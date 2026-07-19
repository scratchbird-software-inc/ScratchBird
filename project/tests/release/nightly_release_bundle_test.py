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
sys.path.insert(0, str(REPO_ROOT / "project" / "tools" / "release"))
import stage_native_release_bundle as native  # noqa: E402


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def native_profile(
    platform: str,
    *,
    native_parser: str = "SBSQL",
    resource_counts: dict[str, int],
    policy_count: int,
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
                    [llvm_runtime["runtime_library"]]
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
            },
            sort_keys=True,
        )
        + "\n"
    ).encode()


def native_resource_fixture(marker: str) -> tuple[dict[str, bytes], dict[str, int], int]:
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
    for rel in native.REQUIRED_OPERABILITY_FILES:
        resources[rel] = f"{marker}:{rel}\n".encode()

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
    return resources, dict(sorted(family_counts.items())), len(policy_rows)


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
    resource_files, resource_counts, policy_count = native_resource_fixture(marker)
    runtime_files = (
        {"opt/ScratchBird/bin/libLLVM-22.dll": b"bundled llvm runtime"}
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
            architecture=profile_architecture or architecture,
            omit_library_role=omit_library_role,
        ),
    }
    if extra_binary is not None:
        files[f"opt/ScratchBird/bin/{extra_binary}{suffix}"] = b"forbidden compatibility binary"
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
    for rel, content in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        rows.append({"path": rel, "bytes": len(content), "sha256": digest(content)})
    rows.sort(key=lambda row: row["path"])
    (root / "INSTALLER_ARTIFACT_MANIFEST.json").write_text(
        json.dumps(
            {
                "schema_id": "scratchbird.installer_artifact_manifest.v1",
                "platform": platform,
                "version": version,
                "build_id": build_id,
                "artifacts": rows,
            },
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
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (root / "SHA256SUMS").write_text(
        "".join(f"{row['sha256']}  {row['path']}\n" for row in manifest["artifacts"]),
        encoding="utf-8",
    )


def make_fixture(input_root: Path, version: str = "1.2.3-nightly") -> None:
    write_installer_artifact(
        input_root / "scratchbird-linux-installers",
        "linux",
        version,
        {
            f"scratchbird-linux-{version}.tar.gz": native_archive("linux", "linux"),
            f"scratchbird_{version.replace('-', '+')}_amd64.deb": b"linux deb",
            "scratchbird-1.2.3-1.nightly.x86_64.rpm": b"linux rpm",
            f"scratchbird-aur-{version}.tar.gz": b"linux aur",
            "rpm-build/SPECS/scratchbird.spec": b"internal rpm spec",
            "aur/scratchbird/PKGBUILD": b"internal aur recipe",
        },
        "42",
    )
    windows_files = {
        f"scratchbird-windows-{version}.zip": native_archive("windows", "windows"),
        f"scratchbird-windows-{version}.msi": b"windows msi",
        "scratchbird.wxs": b"internal wix source",
    }
    write_installer_artifact(
        input_root / "scratchbird-windows-installers",
        "windows",
        version,
        windows_files,
        "42",
    )
    for arch in ("x86_64", "arm64"):
        write_installer_artifact(
            input_root / f"scratchbird-macos-{arch}-installers",
            "macos",
            version,
            {
                f"scratchbird-macos-{version}.tar.gz": native_archive(
                    "macos", f"mac-{arch}", architecture=arch
                ),
                f"scratchbird-macos-{version}.pkg": f"mac pkg {arch}".encode(),
                "MACOS_DYNAMIC_LIBRARY_AUDIT.json": b'{"rows":[{"status":"checked"}]}\n',
                "MACOS_SIGNING_STATE.json": b'{"status":"qa_unsigned_not_for_public_signed_release"}\n',
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

    def create(self, input_root: Path, output_root: Path, *, version: str = "1.2.3-nightly") -> Path:
        return bundle.create_bundle(
            input_root,
            output_root,
            version,
            self.revision,
            "42",
            "1",
        )

    def test_native_flat_bundle_excludes_internal_and_smoke_evidence(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-bundle-") as temp:
            root = Path(temp)
            input_root = root / "input"
            output_root = root / "output"
            make_fixture(input_root)
            manifest_path = self.create(input_root, output_root)
            expected = {
                "scratchbird-nightly-linux-x86_64.tar.gz",
                "scratchbird-nightly-linux-x86_64.deb",
                "scratchbird-nightly-linux-x86_64.rpm",
                "scratchbird-nightly-linux-x86_64-aur.tar.gz",
                "scratchbird-nightly-windows-x86_64.zip",
                "scratchbird-nightly-windows-x86_64.msi",
                "scratchbird-nightly-macos-x86_64.tar.gz",
                "scratchbird-nightly-macos-x86_64.pkg",
                "scratchbird-nightly-macos-arm64.tar.gz",
                "scratchbird-nightly-macos-arm64.pkg",
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
            self.assertEqual("scratchbird_native_no_emulation", manifest["distribution_surface"])
            self.assertEqual("SBSQL", manifest["native_parser"])
            self.assertEqual(
                "fully_verified_native_portable_and_system_installer_artifacts",
                manifest["public_asset_policy"],
            )
            self.assertEqual(
                ["build_recipes", "install_smoke_proof"],
                manifest["internal_only_artifact_classes"],
            )
            verification = {
                row["name"]: row["verification"] for row in manifest["artifacts"]
            }
            self.assertEqual(
                "installer_manifest_and_privileged_deb_smoke",
                verification["scratchbird-nightly-linux-x86_64.deb"],
            )
            self.assertEqual(
                "installer_manifest_and_rpm_recipe_verification",
                verification["scratchbird-nightly-linux-x86_64.rpm"],
            )
            self.assertEqual(
                "installer_manifest_and_aur_recipe_verification",
                verification["scratchbird-nightly-linux-x86_64-aur.tar.gz"],
            )
            self.assertEqual(
                "installer_manifest_and_msi_smoke",
                verification["scratchbird-nightly-windows-x86_64.msi"],
            )
            self.assertEqual(
                "installer_manifest_and_pkg_smoke",
                verification["scratchbird-nightly-macos-x86_64.pkg"],
            )
            self.assertEqual(
                ["SBmgr", "SBgate", "SBParser", "SBsrv"],
                [row["name"] for row in manifest["native_components"]],
            )
            self.assertFalse(manifest["emulation_layers_included"])
            self.assertEqual("system-package", manifest["llvm_runtime"]["linux"]["delivery"])
            self.assertEqual("bundled", manifest["llvm_runtime"]["windows"]["delivery"])
            self.assertEqual(
                "external-homebrew", manifest["llvm_runtime"]["macos"]["delivery"]
            )
            output_text = "\n".join(path.name for path in output_root.iterdir())
            for forbidden in ("smoke", "PKGBUILD", ".wxs", ".spec", "rpm-build"):
                self.assertNotIn(forbidden, output_text)
            sums = bundle.parse_sha256sums(output_root / "scratchbird-nightly-SHA256SUMS")
            for name, expected_digest in sums.items():
                self.assertEqual(expected_digest, bundle.sha256_file(output_root / name))

    def test_verified_system_installers_are_public_release_assets(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-system-installers-") as temp:
            root = Path(temp)
            input_root = root / "input"
            make_fixture(input_root)
            self.create(input_root, root / "output")
            output_names = {path.name for path in (root / "output").iterdir()}
            self.assertEqual(13, len(output_names))
            for name in (
                "scratchbird-nightly-linux-x86_64.deb",
                "scratchbird-nightly-linux-x86_64.rpm",
                "scratchbird-nightly-linux-x86_64-aur.tar.gz",
                "scratchbird-nightly-windows-x86_64.msi",
                "scratchbird-nightly-macos-x86_64.pkg",
                "scratchbird-nightly-macos-arm64.pkg",
            ):
                self.assertIn(name, output_names)

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
            with self.assertRaisesRegex(bundle.BundleError, "package_cardinality:linux"):
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
            with self.assertRaisesRegex(bundle.BundleError, "archive_link_or_device_forbidden"):
                self.create(input_root, root / "output")


if __name__ == "__main__":
    unittest.main(verbosity=2)
