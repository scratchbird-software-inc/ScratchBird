#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Assemble a macOS universal QA tarball from verified per-arch tarballs."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
from typing import Any


METADATA_NAMES = {
    "INSTALL_MANIFEST.json",
    "SHA256SUMS",
    "PUBLIC_RELEASE_ARTIFACT_MANIFEST.json",
    "STANDALONE_OUTPUT_MANIFEST.json",
    "MACOS_SUPPORT_MATRIX.json",
    "NATIVE_RELEASE_PROFILE.json",
}

NATIVE_PROFILE_RELATIVE_PATH = Path(
    "opt/ScratchBird/share/scratchbird/release/NATIVE_RELEASE_PROFILE.json"
)


def fail(message: str) -> None:
    print(f"make_macos_universal=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(command: list[str], cwd: Path) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        print(result.stdout, end="")
        fail(f"command_failed:{command[0]}:exit={result.returncode}")
    return result.stdout


def extract_tarball(tarball: Path, target: Path) -> None:
    target.mkdir(parents=True, exist_ok=True)
    names: set[str] = set()
    total_size = 0
    try:
        with tarfile.open(tarball, "r:gz") as archive:
            for index, member in enumerate(archive, start=1):
                if index > 100_000:
                    fail(f"archive_member_limit:{tarball.name}")
                raw_name = member.name.rstrip("/")
                if not raw_name:
                    continue
                relative = Path(raw_name)
                if (
                    relative.is_absolute()
                    or "\\" in raw_name
                    or any(part in {"", ".", ".."} for part in relative.parts)
                ):
                    fail(f"archive_path_unsafe:{tarball.name}:{member.name}")
                normalized = relative.as_posix()
                if normalized in names:
                    fail(f"archive_path_duplicate:{tarball.name}:{normalized}")
                names.add(normalized)
                destination = target / relative
                if member.issym() or member.islnk() or member.isdev() or member.isfifo():
                    fail(f"archive_special_entry_forbidden:{tarball.name}:{normalized}")
                if member.isdir():
                    destination.mkdir(parents=True, exist_ok=True)
                    destination.chmod(member.mode & 0o777)
                    continue
                if not member.isfile():
                    fail(f"archive_member_type_forbidden:{tarball.name}:{normalized}")
                total_size += member.size
                if member.size < 0 or total_size > 16 * 1024 * 1024 * 1024:
                    fail(f"archive_uncompressed_size_limit:{tarball.name}")
                source = archive.extractfile(member)
                if source is None:
                    fail(f"archive_member_unreadable:{tarball.name}:{normalized}")
                destination.parent.mkdir(parents=True, exist_ok=True)
                with destination.open("wb") as handle:
                    shutil.copyfileobj(source, handle, length=1024 * 1024)
                if destination.stat().st_size != member.size:
                    fail(f"archive_member_size_mismatch:{tarball.name}:{normalized}")
                destination.chmod(member.mode & 0o777)
    except (OSError, tarfile.TarError) as exc:
        fail(f"archive_invalid:{tarball.name}:{exc}")


def is_macho(path: Path) -> bool:
    file_tool = shutil.which("file")
    if not file_tool:
        return path.suffix in {".dylib", ".so"} or path.parent.name == "bin"
    output = run([file_tool, str(path)], cwd=path.parent)
    return "Mach-O" in output


def is_lipo_candidate(path: Path) -> bool:
    if path.suffix == ".a":
        return True
    return is_macho(path)


def is_metadata_path(path: Path) -> bool:
    return path.name in METADATA_NAMES and "share/scratchbird/release" in path.as_posix()


def load_native_profile(root: Path, architecture: str) -> dict[str, Any]:
    path = root / NATIVE_PROFILE_RELATIVE_PATH
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"native_profile_invalid:{architecture}:{exc}")
    if not isinstance(value, dict):
        fail(f"native_profile_not_object:{architecture}")
    if value.get("schema_id") != "scratchbird.native_release_profile.v1":
        fail(f"native_profile_schema_invalid:{architecture}")
    if value.get("profile") != "native-sbsql-only" or value.get("platform") != "macos":
        fail(f"native_profile_identity_invalid:{architecture}")
    return value


def reconcile_native_profile(
    x86_root: Path,
    arm_root: Path,
    universal_root: Path,
) -> None:
    x86_profile = load_native_profile(x86_root, "x86_64")
    arm_profile = load_native_profile(arm_root, "arm64")
    x86_llvm = x86_profile.pop("llvm_runtime", None)
    arm_llvm = arm_profile.pop("llvm_runtime", None)
    if x86_profile != arm_profile:
        fail("native_profile_non_llvm_difference")
    if not isinstance(x86_llvm, dict) or not isinstance(arm_llvm, dict):
        fail("native_profile_llvm_runtime_missing")
    for field, expected in (
        ("link_mode", "dynamic"),
        ("delivery", "external-homebrew"),
    ):
        if x86_llvm.get(field) != expected or arm_llvm.get(field) != expected:
            fail(f"native_profile_llvm_runtime_field_invalid:{field}")
    x86_minimum_major = x86_llvm.get("minimum_major")
    arm_minimum_major = arm_llvm.get("minimum_major")
    if (
        not isinstance(x86_minimum_major, int)
        or isinstance(x86_minimum_major, bool)
        or x86_minimum_major < 22
        or arm_minimum_major != x86_minimum_major
    ):
        fail(
            "native_profile_llvm_runtime_minimum_major_invalid:"
            f"x86_64={x86_minimum_major}:arm64={arm_minimum_major}"
        )
    x86_path = x86_llvm.get("runtime_library")
    arm_path = arm_llvm.get("runtime_library")
    if (
        not isinstance(x86_path, str)
        or not x86_path.startswith("/usr/local/opt/llvm/lib/")
        or not isinstance(arm_path, str)
        or not arm_path.startswith("/opt/homebrew/opt/llvm/lib/")
    ):
        fail(
            "native_profile_llvm_runtime_architecture_path_invalid:"
            f"x86_64={x86_path}:arm64={arm_path}"
        )
    x86_profile["llvm_runtime"] = {
        "link_mode": "dynamic",
        "runtime_library": None,
        "runtime_libraries_by_architecture": {
            "x86_64": x86_path,
            "arm64": arm_path,
        },
        "delivery": "external-homebrew",
        "minimum_major": x86_minimum_major,
    }
    target = universal_root / NATIVE_PROFILE_RELATIVE_PATH
    target.write_text(
        json.dumps(x86_profile, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def collect_files(root: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        rel = path.relative_to(root).as_posix()
        if rel.endswith("INSTALL_MANIFEST.json") or rel.endswith("SHA256SUMS"):
            continue
        rows.append({"path": rel, "bytes": path.stat().st_size, "sha256": sha256_file(path)})
    return rows


def rewrite_payload_metadata(root: Path, version: str, build_id: str | None) -> None:
    release_dir = root / "opt" / "ScratchBird" / "share" / "scratchbird" / "release"
    release_dir.mkdir(parents=True, exist_ok=True)
    for stale_name in ("PUBLIC_RELEASE_ARTIFACT_MANIFEST.json", "STANDALONE_OUTPUT_MANIFEST.json"):
        (release_dir / stale_name).unlink(missing_ok=True)
    (release_dir / "MACOS_UNIVERSAL_MANIFEST.json").write_text(
        json.dumps(
            {
                "schema_id": "scratchbird.macos_universal_manifest.v1",
                "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
                "architectures": ["x86_64", "arm64"],
                "assembly_tool": "lipo",
                "artifact_policy": "qa_universal_after_per_architecture_artifacts_verify",
                "version": version,
                "build_id": build_id,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    rows = collect_files(root)
    install_manifest = {
        "schema_id": "scratchbird.installer_payload_manifest.v1",
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "product": "ScratchBird",
        "platform": "macos",
        "architectures": ["x86_64", "arm64"],
        "version": version,
        "build_id": build_id,
        "install_roots": {
            "runtime": "/opt/ScratchBird",
            "configuration": "/etc/scratchbird",
        },
        "files": rows,
    }
    (release_dir / "INSTALL_MANIFEST.json").write_text(
        json.dumps(install_manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (release_dir / "SHA256SUMS").write_text(
        "\n".join(f"{row['sha256']}  {row['path']}" for row in rows) + "\n",
        encoding="utf-8",
    )


def make_tarball(payload_root: Path, output_root: Path, version: str) -> Path:
    output = output_root / f"scratchbird-macos-universal-{version}.tar.gz"
    with tarfile.open(output, "w:gz", format=tarfile.PAX_FORMAT) as archive:
        for path in sorted(payload_root.rglob("*")):
            archive.add(path, arcname=path.relative_to(payload_root).as_posix(), recursive=False)
    return output


def write_manifest(output_root: Path, tarball: Path, x86_tar: Path, arm_tar: Path, version: str, build_id: str | None) -> Path:
    payload = {
        "schema_id": "scratchbird.macos_universal_artifact_manifest.v1",
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "version": version,
        "build_id": build_id,
        "inputs": {
            "x86_64": {"path": x86_tar.name, "sha256": sha256_file(x86_tar)},
            "arm64": {"path": arm_tar.name, "sha256": sha256_file(arm_tar)},
        },
        "artifact": {
            "path": tarball.name,
            "bytes": tarball.stat().st_size,
            "sha256": sha256_file(tarball),
            "architectures": ["x86_64", "arm64"],
            "status": "qa_universal_after_per_architecture_artifacts_verify",
        },
    }
    path = output_root / "MACOS_UNIVERSAL_ARTIFACT_MANIFEST.json"
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


def assemble(x86_root: Path, arm_root: Path, universal_root: Path) -> None:
    shutil.copytree(x86_root, universal_root)
    x86_files = {path.relative_to(x86_root).as_posix(): path for path in x86_root.rglob("*") if path.is_file()}
    arm_files = {path.relative_to(arm_root).as_posix(): path for path in arm_root.rglob("*") if path.is_file()}
    if set(x86_files) != set(arm_files):
        missing_x86 = sorted(set(arm_files) - set(x86_files))
        missing_arm = sorted(set(x86_files) - set(arm_files))
        fail(f"payload_file_set_mismatch:x86_missing={missing_x86[:5]}:arm_missing={missing_arm[:5]}")
    lipo = shutil.which("lipo")
    if not lipo:
        fail("lipo_not_found")
    for rel, x86_path in sorted(x86_files.items()):
        arm_path = arm_files[rel]
        output_path = universal_root / rel
        if sha256_file(x86_path) == sha256_file(arm_path):
            continue
        if is_metadata_path(x86_path):
            continue
        if is_lipo_candidate(x86_path) and is_lipo_candidate(arm_path):
            run([lipo, "-create", "-output", str(output_path), str(x86_path), str(arm_path)], cwd=universal_root)
            continue
        fail(f"non_universal_payload_difference:{rel}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--x86-tar", type=Path, required=True)
    parser.add_argument("--arm-tar", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--version", default="0.0.0-nightly")
    parser.add_argument("--build-id")
    args = parser.parse_args()

    output_root = args.output_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="scratchbird-macos-universal-") as temp:
        temp_root = Path(temp)
        x86_root = temp_root / "x86_64"
        arm_root = temp_root / "arm64"
        universal_root = temp_root / "universal"
        extract_tarball(args.x86_tar.resolve(), x86_root)
        extract_tarball(args.arm_tar.resolve(), arm_root)
        assemble(x86_root, arm_root, universal_root)
        reconcile_native_profile(x86_root, arm_root, universal_root)
        rewrite_payload_metadata(universal_root, args.version, args.build_id)
        tarball = make_tarball(universal_root, output_root, args.version)
    manifest = write_manifest(output_root, tarball, args.x86_tar.resolve(), args.arm_tar.resolve(), args.version, args.build_id)
    print(f"make_macos_universal=passed:{manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
