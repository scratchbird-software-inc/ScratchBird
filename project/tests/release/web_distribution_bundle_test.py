#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Exercise the native-server-only webserver package export generator."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import zipfile


REPO_ROOT = Path(__file__).resolve().parents[3]
RELEASE_TOOLS = REPO_ROOT / "project" / "tools" / "release"
if str(RELEASE_TOOLS) not in sys.path:
    sys.path.insert(0, str(RELEASE_TOOLS))
import stage_native_release_bundle as native  # noqa: E402


NATIVE_PROFILE_PATH = (
    "opt/ScratchBird/share/scratchbird/release/NATIVE_RELEASE_PROFILE.json"
)
INSTALL_MANIFEST_PATH = (
    "opt/ScratchBird/share/scratchbird/release/INSTALL_MANIFEST.json"
)
INSTALL_SHA256SUMS_PATH = "opt/ScratchBird/share/scratchbird/release/SHA256SUMS"
NATIVE_SERVER_ADMISSION = {
    "schema_id": "scratchbird.installer_native_server_admission.v1",
    "distribution_surface": "scratchbird_native_no_emulation",
    "admission_controller": "native_server_only",
    "client_artifacts_permitted": False,
    "admitted_driver_adaptor_mcp_components": [],
    "dbeaver_hard_excluded": True,
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        digest.update(handle.read())
    return digest.hexdigest()


def write_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def write_json(path: Path, data: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


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
    # The installed-payload verifier binds docs/examples to the exact public
    # source inventory.  Preserve that closed inventory in the web fixture,
    # including the reviewed non-payload driver-route example script.
    nonresource_inventory: list[dict[str, str]] = []
    for rel, source_rel in sorted(native.NATIVE_SHARE_NONRESOURCE_SOURCE_FILES.items()):
        value = (native.PUBLIC_REPO_ROOT / source_rel).read_bytes()
        resources[rel] = value
        nonresource_inventory.append(
            {"path": rel, "sha256": hashlib.sha256(value).hexdigest()}
        )

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
    for rel in sorted(
        path for path in resources if path.startswith(seed_prefix + "resources/")
    ):
        seed_rel = rel.removeprefix(seed_prefix)
        content = resources[rel].replace(b"\r\n", b"\n").replace(b"\r", b"\n")
        seed_rows.append((seed_rel, native.fnv1a64(content), len(content)))
        parts = Path(seed_rel).parts
        family = parts[1] if len(parts) > 1 and parts[0] == "resources" else "other"
        family_counts[family] = family_counts.get(family, 0) + 1
    resources[seed_artifacts] = (
        "canonical_path,content_hash,content_size_bytes\n"
        + "".join(
            f"{rel},{content_hash},{size}\n"
            for rel, content_hash, size in seed_rows
        )
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


def native_profile(
    platform: str,
    resource_counts: dict[str, int],
    policy_count: int,
    nonresource_inventory: list[dict[str, str]],
    architecture: str | None,
) -> dict[str, object]:
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
    for candidates in native.REQUIRED_LIBRARY_CANDIDATES[platform].values():
        name = candidates[0]
        libraries.append(f"{'bin' if name.endswith('.dll') else 'lib'}/{name}")
    return {
        "schema_id": "scratchbird.native_release_profile.v1",
        "profile": "native-sbsql-only",
        "platform": platform,
        "native_parser": "SBSQL",
        "emulation_components": "excluded",
        "executables": sorted(
            f"{name}{suffix}" for name in native.native_executables(platform)
        ),
        "libraries": sorted(libraries),
        "runtime_dependencies": (
            sorted(native.WINDOWS_NATIVE_RUNTIME_NAMES) if platform == "windows" else []
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
    }


def canonical_json_bytes(data: object) -> bytes:
    return (json.dumps(data, indent=2, sort_keys=True) + "\n").encode("utf-8")


def write_native_archive(
    path: Path,
    platform: str,
    *,
    architecture: str | None = None,
    extra_entries: dict[str, bytes] | None = None,
) -> str:
    marker = f"webdist:{platform}:{architecture or 'default'}"
    suffix = ".exe" if platform == "windows" else ""
    (
        resource_files,
        resource_counts,
        policy_count,
        nonresource_inventory,
    ) = native_resource_fixture(marker)
    profile_bytes = canonical_json_bytes(
        native_profile(
            platform,
            resource_counts,
            policy_count,
            nonresource_inventory,
            architecture,
        )
    )
    libraries: dict[str, bytes] = {}
    for candidates in native.REQUIRED_LIBRARY_CANDIDATES[platform].values():
        name = candidates[0]
        directory = "bin" if name.endswith(".dll") else "lib"
        libraries[f"opt/ScratchBird/{directory}/{name}"] = f"{marker}:{name}".encode()
    runtime_files = (
        {
            f"opt/ScratchBird/bin/{name}": f"{marker}:{name}".encode()
            for name in native.WINDOWS_NATIVE_RUNTIME_NAMES
        }
        if platform == "windows"
        else {}
    )
    entries = {
        **{
            f"opt/ScratchBird/bin/{name}{suffix}": f"{marker}:{name}".encode()
            for name in native.native_executables(platform)
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
        NATIVE_PROFILE_PATH: profile_bytes,
    }
    if extra_entries:
        entries.update(extra_entries)
    rows = [
        {
            "path": rel,
            "bytes": len(contents),
            "sha256": hashlib.sha256(contents).hexdigest(),
        }
        for rel, contents in sorted(entries.items())
    ]
    entries[INSTALL_MANIFEST_PATH] = canonical_json_bytes(
        {
            "schema_id": "scratchbird.installer_payload_manifest.v1",
            "platform": platform,
            "files": rows,
        }
    )
    entries[INSTALL_SHA256SUMS_PATH] = "".join(
        f"{row['sha256']}  {row['path']}\n" for row in rows
    ).encode()
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.name.endswith(".tar.gz"):
        with tarfile.open(path, "w:gz", format=tarfile.PAX_FORMAT) as archive:
            for rel, contents in sorted(entries.items()):
                info = tarfile.TarInfo(rel)
                info.size = len(contents)
                info.mode = 0o644
                archive.addfile(info, io.BytesIO(contents))
    elif path.suffix == ".zip":
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for rel, contents in sorted(entries.items()):
                archive.writestr(rel, contents)
    else:
        raise ValueError(f"unsupported archive fixture:{path}")
    return hashlib.sha256(profile_bytes).hexdigest()


def make_installer_fixture(
    root: Path,
    platform: str,
    files: tuple[str, ...],
    *,
    archive_extra_entries: dict[str, bytes] | None = None,
) -> None:
    profile_digest: str | None = None
    rows = []
    for rel in files:
        path = root / rel
        if path.name.endswith(".tar.gz") or path.suffix == ".zip":
            digest = write_native_archive(
                path,
                platform,
                extra_entries=archive_extra_entries,
            )
            if profile_digest is None:
                profile_digest = digest
            elif profile_digest != digest:
                raise AssertionError("fixture profile digest mismatch")
        else:
            write_bytes(path, b"verified system package fixture")
        rows.append({"path": rel, "bytes": path.stat().st_size, "sha256": sha256_file(path)})
    if profile_digest is None:
        raise AssertionError("fixture needs a portable native archive")
    manifest = {
        "schema_id": "scratchbird.installer_artifact_manifest.v1",
        "platform": platform,
        "version": "1.2.3-beta",
        "build_id": "test-run",
        "native_server_admission": {
            **NATIVE_SERVER_ADMISSION,
            "native_release_profile_sha256": profile_digest,
        },
        "artifacts": rows,
    }
    if platform == "windows":
        manifest["windows"] = {
            "package_mode": "portable_zip_only",
            "system_installer_included": False,
            "portable_archive_smoke_required": True,
            "native_default_port": 3092,
        }
    write_json(root / "INSTALLER_ARTIFACT_MANIFEST.json", manifest)
    (root / "SHA256SUMS").write_text(
        "".join(f"{row['sha256']}  {row['path']}\n" for row in rows),
        encoding="utf-8",
    )


def run_export(script: Path, repo_root: Path, input_root: Path, output_root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(script),
            "--input-root",
            str(input_root),
            "--output-root",
            str(output_root),
            "--version",
            "1.2.3-beta",
            "--channel",
            "beta",
            "--base-url",
            "https://downloads.example.invalid",
            "--source-revision",
            "abc123",
            "--github-run-id",
            "42",
        ],
        cwd=repo_root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, required=True)
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()
    script = repo_root / "project" / "tools" / "installers" / "create_web_distribution_bundle.py"
    if not script.is_file():
        print(f"missing_script:{script}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="sb-webdist-test-") as temp:
        temp_root = Path(temp)
        input_root = temp_root / "input"
        output_root = temp_root / "output"
        make_installer_fixture(
            input_root / "scratchbird-linux-installers",
            "linux",
            ("scratchbird-linux-1.2.3-beta.tar.gz",),
        )
        make_installer_fixture(
            input_root / "scratchbird-windows-installers",
            "windows",
            ("scratchbird-windows-1.2.3-beta.zip",),
        )
        universal_root = input_root / "scratchbird-macos-universal-installers"
        universal_artifact = universal_root / "scratchbird-macos-universal-1.2.3-beta.tar.gz"
        universal_profile_digest = write_native_archive(
            universal_artifact,
            "macos",
            architecture="universal",
        )
        write_json(
            universal_root / "MACOS_UNIVERSAL_ARTIFACT_MANIFEST.json",
            {
                "schema_id": "scratchbird.macos_universal_artifact_manifest.v1",
                "version": "1.2.3-beta",
                "build_id": "test-run",
                "native_server_admission": {
                    **NATIVE_SERVER_ADMISSION,
                    "native_release_profile_sha256": universal_profile_digest,
                },
                "artifact": {
                    "path": universal_artifact.name,
                    "bytes": universal_artifact.stat().st_size,
                    "sha256": sha256_file(universal_artifact),
                    "architectures": ["x86_64", "arm64"],
                    "status": "qa_universal_after_per_architecture_artifacts_verify",
                },
            },
        )

        completed = run_export(script, repo_root, input_root, output_root)
        if completed.returncode != 0:
            print(completed.stdout, end="")
            return completed.returncode

        version_root = output_root / "beta" / "1.2.3-beta"
        expected = [
            version_root / "linux" / "x86_64" / "scratchbird-linux-1.2.3-beta.tar.gz",
            version_root / "windows" / "x86_64" / "scratchbird-windows-1.2.3-beta.zip",
            version_root / "macos" / "universal" / "scratchbird-macos-universal-1.2.3-beta.tar.gz",
            version_root / "WEB_DISTRIBUTION_MANIFEST.json",
            version_root / "SHA256SUMS",
            version_root / "UPLOAD_LAYOUT.txt",
        ]
        missing = [path.relative_to(version_root).as_posix() for path in expected if not path.is_file()]
        if missing:
            print(f"missing_outputs:{missing}", file=sys.stderr)
            return 1
        manifest = json.loads((version_root / "WEB_DISTRIBUTION_MANIFEST.json").read_text(encoding="utf-8"))
        if manifest.get("publication_policy") != "webserver_upload_only_no_github_release":
            print("publication_policy_mismatch", file=sys.stderr)
            return 1
        admission = manifest.get("native_server_admission")
        if (
            not isinstance(admission, dict)
            or admission.get("client_artifacts_permitted") is not False
            or admission.get("admitted_driver_adaptor_mcp_components") != []
            or admission.get("dbeaver_hard_excluded") is not True
        ):
            print("web_native_server_admission_missing", file=sys.stderr)
            return 1
        paths = {row["path"] for row in manifest.get("artifacts", [])}
        required_paths = {
            "linux/x86_64/scratchbird-linux-1.2.3-beta.tar.gz",
            "windows/x86_64/scratchbird-windows-1.2.3-beta.zip",
            "macos/universal/scratchbird-macos-universal-1.2.3-beta.tar.gz",
        }
        if not required_paths.issubset(paths):
            print(f"manifest_paths_missing:{sorted(required_paths - paths)}", file=sys.stderr)
            return 1

        reject_input = temp_root / "reject-input"
        make_installer_fixture(
            reject_input / "scratchbird-windows-installers",
            "windows",
            (
                "scratchbird-windows-1.2.3-beta.zip",
                "scratchbird-windows-1.2.3-beta.msi",
            ),
        )
        rejected = run_export(script, repo_root, reject_input, temp_root / "reject-output")
        if rejected.returncode == 0 or "windows_zip_only_forbidden_artifact" not in rejected.stdout:
            print(f"windows_msi_web_export_not_rejected:{rejected.stdout}", file=sys.stderr)
            return 1

        hidden_payload_input = temp_root / "hidden-payload-input"
        make_installer_fixture(
            hidden_payload_input / "scratchbird-linux-installers",
            "linux",
            ("scratchbird-linux-1.2.3-beta.tar.gz",),
            archive_extra_entries={
                "opt/ScratchBird/extensions/enginehelper.bin": b"\x7fELFneutral-client"
            },
        )
        rejected = run_export(
            script,
            repo_root,
            hidden_payload_input,
            temp_root / "hidden-payload-output",
        )
        if (
            rejected.returncode == 0
            or "native_admission_payload_layout_" not in rejected.stdout
        ):
            print(
                f"neutral_hidden_payload_web_export_not_rejected:{rejected.stdout}",
                file=sys.stderr,
            )
            return 1

        unmanifested_input = temp_root / "unmanifested-input"
        unmanifested_root = unmanifested_input / "scratchbird-linux-installers"
        make_installer_fixture(
            unmanifested_root,
            "linux",
            ("scratchbird-linux-1.2.3-beta.tar.gz",),
        )
        write_bytes(unmanifested_root / "neutralhelper.bin", b"\x7fELFunmanifested")
        rejected = run_export(
            script,
            repo_root,
            unmanifested_input,
            temp_root / "unmanifested-output",
        )
        if (
            rejected.returncode == 0
            or "web_input_artifact_root_content_mismatch" not in rejected.stdout
        ):
            print(
                f"unmanifested_artifact_root_file_not_rejected:{rejected.stdout}",
                file=sys.stderr,
            )
            return 1

        system_package_input = temp_root / "system-package-input"
        make_installer_fixture(
            system_package_input / "scratchbird-linux-installers",
            "linux",
            (
                "scratchbird-linux-1.2.3-beta.tar.gz",
                "scratchbird_1.2.3_beta_amd64.deb",
            ),
        )
        rejected = run_export(
            script,
            repo_root,
            system_package_input,
            temp_root / "system-package-output",
        )
        if rejected.returncode == 0 or "portable_artifact_cardinality" not in rejected.stdout:
            print(f"system_package_web_export_not_rejected:{rejected.stdout}", file=sys.stderr)
            return 1

        aur_input = temp_root / "aur-input"
        make_installer_fixture(
            aur_input / "scratchbird-linux-installers",
            "linux",
            ("scratchbird-aur-1.2.3-beta.tar.gz",),
        )
        rejected = run_export(script, repo_root, aur_input, temp_root / "aur-output")
        if rejected.returncode == 0 or "portable_archive_name_invalid" not in rejected.stdout:
            print(f"aur_web_export_not_rejected:{rejected.stdout}", file=sys.stderr)
            return 1

        cross_platform_input = temp_root / "cross-platform-input"
        cross_platform_root = cross_platform_input / "scratchbird-linux-installers"
        make_installer_fixture(
            cross_platform_root,
            "linux",
            ("scratchbird-linux-1.2.3-beta.tar.gz",),
        )
        cross_platform_manifest = json.loads(
            (cross_platform_root / "INSTALLER_ARTIFACT_MANIFEST.json").read_text(
                encoding="utf-8"
            )
        )
        cross_platform_manifest["artifacts"][0]["path"] = "C:/payload.tar.gz"
        write_json(
            cross_platform_root / "INSTALLER_ARTIFACT_MANIFEST.json",
            cross_platform_manifest,
        )
        rejected = run_export(
            script,
            repo_root,
            cross_platform_input,
            temp_root / "cross-platform-output",
        )
        if (
            rejected.returncode == 0
            or "cross_platform_absolute_path_forbidden" not in rejected.stdout
        ):
            print(f"cross_platform_path_not_rejected:{rejected.stdout}", file=sys.stderr)
            return 1

        unknown_input = temp_root / "unknown-input"
        make_installer_fixture(
            unknown_input / "scratchbird-driver-installers",
            "linux",
            ("scratchbird-linux-1.2.3-beta.tar.gz",),
        )
        rejected = run_export(script, repo_root, unknown_input, temp_root / "unknown-output")
        if rejected.returncode == 0 or "web_input_artifact_root_unadmitted" not in rejected.stdout:
            print(f"unknown_artifact_root_not_rejected:{rejected.stdout}", file=sys.stderr)
            return 1
    print("web_distribution_bundle_test=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
