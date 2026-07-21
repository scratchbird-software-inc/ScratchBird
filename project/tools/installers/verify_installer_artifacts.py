#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Verify ScratchBird installer artifact manifests and boundary rules."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
import tempfile
from typing import Any
import zipfile
import xml.etree.ElementTree as ET


TOOL_ROOT = Path(__file__).resolve().parent
if str(TOOL_ROOT) not in sys.path:
    sys.path.insert(0, str(TOOL_ROOT))

import installer_native_admission as native_admission


MANIFEST_NAME = "INSTALLER_ARTIFACT_MANIFEST.json"
FORBIDDEN_TEXT = (
    "ScratchBird" + "-Private",
    "/home/",
    "\\home\\",
    "/local" + "_work",
    "\\local" + "_work",
    "docs/workplans",
    "docs/specifications",
    "project/tests/reference_regression/reference_release_acquisition/",
    "packaging/",
)
REQUIRED_SUFFIXES = {
    "linux": (".tar.gz",),
    "windows": (".zip",),
    "macos": (".tar.gz",),
}
PUBLICATION_SURFACE_SCHEMA = "scratchbird.public_native_installer_artifact.v1"
PUBLICATION_SURFACE_POLICY = (
    "exact_manifest_derived_directly_recursive_verifiable_native_server_payloads_only"
)
EXCLUDED_PUBLIC_PACKAGE_FORMATS = ("rpm", "pkg", "deb", "aur", "msi")
PUBLIC_PACKAGE_PATTERNS = {
    "linux": ("scratchbird-linux-*.tar.gz",),
    "windows": ("scratchbird-windows-*.zip",),
    "macos": ("scratchbird-macos-*.tar.gz",),
}

MACOS_REQUIRED_SIDECARS = (
    "MACOS_DYNAMIC_LIBRARY_AUDIT.json",
    "MACOS_SIGNING_STATE.json",
)
WINDOWS_MSI_REQUIRED_SIDECARS = (
    "WINDOWS_SYSTEM_PACKAGE_EVIDENCE.json",
    "scratchbird.wxs",
    "scratchbird-windows-lifecycle.wxs",
)
WINDOWS_ZIP_ONLY_FORBIDDEN_NAMES = frozenset(
    {
        "WINDOWS_SYSTEM_PACKAGE_EVIDENCE.json",
        "scratchbird.wxs",
        "scratchbird-windows-lifecycle.wxs",
    }
)
WINDOWS_ZIP_ONLY_FORBIDDEN_SUFFIXES = frozenset({".msi", ".wixpdb", ".wxs"})
SERVICE_AUTHORITY_SCOPE = (
    "filesystem_directory_and_process_execution_only_"
    "no_database_or_security_authority"
)
WIX_NAMESPACE = {"w": "http://wixtoolset.org/schemas/v4/wxs"}
WIX_DATA_NAMESPACE = {
    "w": "http://wixtoolset.org/schemas/v4/windowsinstallerdata"
}
WIX_PDB_DATA_ENTRY = "wix-wid.xml"
WINDOWS_LIFECYCLE_ACTIONS = (
    "ScratchBirdPostInstall",
    "ScratchBirdPreRemove",
)
WINDOWS_LIFECYCLE_SETTERS = (
    "SetScratchBirdPostInstall",
    "SetScratchBirdPreRemove",
)


def native_admission_call(callable_: Any, *args: Any, **kwargs: Any) -> Any:
    try:
        return callable_(*args, **kwargs)
    except native_admission.NativeAdmissionError as exc:
        fail(str(exc))


def artifact_file_rows(
    root: Path,
    artifacts: list[Any],
) -> dict[str, Path]:
    """Verify manifest rows and reject hidden client paths before package use."""

    files: dict[str, Path] = {}
    for row in artifacts:
        if not isinstance(row, dict) or set(row) != {"path", "bytes", "sha256"}:
            fail("manifest_row_not_object")
        rel = row.get("path")
        if not isinstance(rel, str):
            fail("manifest_row_path_invalid")
        relative = native_admission_call(
            native_admission.safe_relative_path, rel, "installer_manifest"
        )
        rel = relative.as_posix()
        if rel in files:
            fail(f"manifest_row_path_duplicate:{rel}")
        path = root / relative
        if not path.is_file() or path.is_symlink():
            fail(f"manifest_file_missing:{rel}")
        if row.get("bytes") != path.stat().st_size:
            fail(f"manifest_size_mismatch:{rel}")
        if row.get("sha256") != sha256_file(path):
            fail(f"manifest_sha256_mismatch:{rel}")
        files[rel] = path
    return files


def verify_manifest_checksums(root: Path, files: dict[str, Path]) -> None:
    checksum_path = root / "SHA256SUMS"
    if not checksum_path.is_file() or checksum_path.is_symlink():
        fail("sha256sums_missing")
    try:
        lines = checksum_path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        fail(f"sha256sums_unreadable:{exc}")
    actual: dict[str, str] = {}
    for line in lines:
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if match is None:
            fail(f"sha256sums_row_invalid:{line}")
        digest, relative = match.groups()
        relative_path = native_admission_call(
            native_admission.safe_relative_path, relative, "sha256sums"
        )
        normalized = relative_path.as_posix()
        if normalized in actual:
            fail(f"sha256sums_duplicate:{normalized}")
        actual[normalized] = digest
    expected = {relative: sha256_file(path) for relative, path in files.items()}
    if actual != expected:
        fail("sha256sums_manifest_mismatch")


def select_manifest_package(
    files: dict[str, Path],
    pattern: str,
    context: str,
) -> tuple[str, Path]:
    matches = sorted(
        (rel, path)
        for rel, path in files.items()
        if "/" not in rel and Path(rel).match(pattern)
    )
    if len(matches) != 1:
        fail(f"native_admission_package_cardinality:{context}:{pattern}:{len(matches)}")
    return matches[0]


def platform_architecture(platform: str, root: Path, supplied: str | None) -> str:
    if supplied is not None:
        return supplied
    if platform in {"linux", "windows"}:
        return "x86_64"
    name = root.name.casefold()
    if "arm64" in name:
        return "arm64"
    if "x86_64" in name or "x64" in name or "intel" in name:
        return "x86_64"
    fail("native_admission_macos_architecture_required")


def verify_native_admission_packages(
    root: Path,
    data: dict[str, Any],
    files: dict[str, Path],
    platform: str,
    architecture: str,
) -> None:
    admission, profile_digest = native_admission_call(
        native_admission.require_native_server_admission,
        data.get("native_server_admission"),
        f"installer:{root.name}",
    )
    native_admission_call(
        native_admission.scan_native_only_tree, root, f"installer_root:{root.name}"
    )
    if platform == "linux":
        rel, path = select_manifest_package(files, "scratchbird-linux-*.tar.gz", "linux:portable")
        native_admission_call(
            native_admission.verify_portable_native_payload,
            path, profile_digest, platform, architecture,
        )
        return
    if platform == "windows":
        rel, path = select_manifest_package(files, "scratchbird-windows-*.zip", "windows:zip")
        native_admission_call(
            native_admission.verify_portable_native_payload,
            path, profile_digest, platform, architecture,
        )
        return
    rel, path = select_manifest_package(files, "scratchbird-macos-*.tar.gz", "macos:portable")
    native_admission_call(
        native_admission.verify_portable_native_payload,
        path, profile_digest, platform, architecture,
    )


def publication_package_rows(
    files: dict[str, Path],
    platform: str,
) -> dict[str, Path]:
    """Return the exact portable package set admitted to tester downloads."""

    selected: dict[str, Path] = {}
    for pattern in PUBLIC_PACKAGE_PATTERNS[platform]:
        relative, path = select_manifest_package(
            files, pattern, f"public:{platform}:{pattern}"
        )
        selected[relative] = path
    return selected


def assert_exact_publication_tree(root: Path, files: dict[str, Path]) -> None:
    """Require an artifact root to contain only its manifest-derived files."""

    expected_files = set(files) | {MANIFEST_NAME, "SHA256SUMS"}
    actual_files = {
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file()
    }
    if actual_files != expected_files:
        fail(
            "public_installer_artifact_tree_mismatch:"
            f"missing={sorted(expected_files - actual_files)}:"
            f"unexpected={sorted(actual_files - expected_files)}"
        )
    expected_dirs: set[str] = set()
    for relative in expected_files:
        parent = Path(relative).parent
        while parent != Path("."):
            expected_dirs.add(parent.as_posix())
            parent = parent.parent
    actual_dirs = {
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_dir()
    }
    if actual_dirs != expected_dirs:
        fail(
            "public_installer_artifact_directory_mismatch:"
            f"missing={sorted(expected_dirs - actual_dirs)}:"
            f"unexpected={sorted(actual_dirs - expected_dirs)}"
        )


def require_publication_manifest(
    root: Path,
    data: dict[str, Any],
    files: dict[str, Path],
    platform: str,
) -> None:
    if data.get("publication_surface") != PUBLICATION_SURFACE_SCHEMA:
        fail(f"public_installer_artifact_surface_missing:{root}")
    if data.get("publication_policy") != PUBLICATION_SURFACE_POLICY:
        fail(f"public_installer_artifact_policy_invalid:{root}")
    if data.get("excluded_package_formats") != list(EXCLUDED_PUBLIC_PACKAGE_FORMATS):
        fail(f"public_installer_artifact_exclusion_invalid:{root}")
    selected = publication_package_rows(files, platform)
    if set(files) != set(selected):
        fail(
            "public_installer_artifact_manifest_set_invalid:"
            f"expected={sorted(selected)}:actual={sorted(files)}"
        )
    assert_exact_publication_tree(root, files)


def materialize_publication_root(
    source_root: Path,
    output_root: Path,
    data: dict[str, Any],
    files: dict[str, Path],
    platform: str,
) -> Path:
    """Write a clean, exact tester-downloadable installer artifact directory.

    Builder directories deliberately retain internal recipes, smoke evidence,
    and package-manager output.  They must never be uploaded wholesale.  This
    function copies only the recursively verified portable package plus an
    exact new manifest/checksum pair, so an unmanifested neutral binary and
    every system-package format are absent before the upload step begins.
    """

    if output_root.is_symlink() or output_root.exists():
        fail(f"public_installer_output_root_not_empty:{output_root}")
    if output_root.resolve() == source_root.resolve() or source_root.resolve() in output_root.resolve().parents:
        fail("public_installer_output_overlap")
    selected = publication_package_rows(files, platform)
    parent = output_root.parent
    parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=f".{output_root.name}.public-", dir=parent
    ) as temp_name:
        staging = Path(temp_name) / "artifact"
        staging.mkdir()
        rows: list[dict[str, Any]] = []
        for relative, source in sorted(selected.items()):
            target = staging / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            rows.append(
                {
                    "path": relative,
                    "bytes": target.stat().st_size,
                    "sha256": sha256_file(target),
                }
            )
        rows.sort(key=lambda row: str(row["path"]))
        manifest: dict[str, Any] = {
            "schema_id": "scratchbird.installer_artifact_manifest.v1",
            "platform": platform,
            "version": data.get("version"),
            "build_id": data.get("build_id"),
            "native_server_admission": data.get("native_server_admission"),
            "publication_surface": PUBLICATION_SURFACE_SCHEMA,
            "publication_policy": PUBLICATION_SURFACE_POLICY,
            "excluded_package_formats": list(EXCLUDED_PUBLIC_PACKAGE_FORMATS),
            "artifacts": rows,
        }
        if platform == "windows":
            manifest["windows"] = data.get("windows")
        if platform == "macos":
            signing = files.get("MACOS_SIGNING_STATE.json")
            if signing is None:
                fail("public_installer_macos_signing_state_missing")
            try:
                status = json.loads(signing.read_text(encoding="utf-8")).get("status")
            except (OSError, json.JSONDecodeError) as exc:
                fail(f"public_installer_macos_signing_state_invalid:{exc}")
            if status not in {"qa_unsigned_not_for_public_signed_release", "payload_signed"}:
                fail(f"public_installer_macos_signing_state_invalid:{status}")
            manifest["macos_signing_status"] = status
        (staging / MANIFEST_NAME).write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        (staging / "SHA256SUMS").write_text(
            "".join(f"{row['sha256']}  {row['path']}\n" for row in rows),
            encoding="utf-8",
        )
        output_root.parent.mkdir(parents=True, exist_ok=True)
        staging.rename(output_root)
    return output_root


def fail(message: str) -> None:
    print(f"verify_installer_artifacts=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_text_candidate(path: Path) -> bool:
    return path.stat().st_size <= 2 * 1024 * 1024 and (
        path.suffix.lower() in {".json", ".md", ".txt", ".csv", ".xml", ".wxs", ".spec", ".service", ".plist", ".sh", ".ps1"}
        or path.name in {"SHA256SUMS", "PKGBUILD"}
    )


def scan(root: Path) -> None:
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        rel = path.relative_to(root).as_posix()
        for fragment in FORBIDDEN_TEXT:
            if fragment in rel:
                fail(f"forbidden_path_fragment:{rel}:{fragment}")
        if not is_text_candidate(path):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for fragment in FORBIDDEN_TEXT:
            if fragment in text:
                fail(f"forbidden_text_fragment:{rel}:{fragment}")


def verify_windows_msi_lifecycle_wiring(root: Path) -> None:
    """Verify that the emitted WiX sources link the lifecycle into the MSI.

    WiX drops unreferenced Fragment sections even when their .wxs file is
    supplied on the command line. Checking the emitted sources before the
    smoke step prevents an MSI that merely ships the lifecycle helper without
    ever executing it.
    """

    try:
        package_tree = ET.parse(root / "scratchbird.wxs")
        lifecycle_tree = ET.parse(root / "scratchbird-windows-lifecycle.wxs")
    except (OSError, ET.ParseError) as exc:
        fail(f"windows_msi_lifecycle_wix_invalid:{exc}")

    package_refs = {
        row.get("Id")
        for row in package_tree.findall(".//w:CustomActionRef", WIX_NAMESPACE)
    }
    if not set(WINDOWS_LIFECYCLE_ACTIONS).issubset(package_refs):
        fail("windows_msi_lifecycle_fragment_unlinked")

    fragments = lifecycle_tree.findall(".//w:Fragment", WIX_NAMESPACE)
    if len(fragments) != 1:
        fail("windows_msi_lifecycle_fragment_split")
    lifecycle_actions = {
        row.get("Id")
        for row in lifecycle_tree.findall(".//w:CustomAction", WIX_NAMESPACE)
    }
    if not set(WINDOWS_LIFECYCLE_ACTIONS).issubset(lifecycle_actions):
        fail("windows_msi_lifecycle_actions_missing")
    scheduled_actions = {
        row.get("Action")
        for row in lifecycle_tree.findall(
            ".//w:InstallExecuteSequence/w:Custom", WIX_NAMESPACE
        )
    }
    if not set(WINDOWS_LIFECYCLE_ACTIONS).issubset(scheduled_actions):
        fail("windows_msi_lifecycle_schedule_missing")


def wix_table_rows(tree: ET.Element, table_name: str) -> list[list[str]]:
    table = tree.find(f'w:table[@name="{table_name}"]', WIX_DATA_NAMESPACE)
    if table is None:
        return []
    return [
        [field.text or "" for field in row.findall("w:field", WIX_DATA_NAMESPACE)]
        for row in table.findall("w:row", WIX_DATA_NAMESPACE)
    ]


def read_wix_linked_database(pdb_path: Path) -> ET.Element:
    try:
        with zipfile.ZipFile(pdb_path) as archive:
            data = archive.read(WIX_PDB_DATA_ENTRY)
        tree = ET.fromstring(data)
    except (OSError, KeyError, zipfile.BadZipFile, ET.ParseError) as exc:
        fail(f"windows_msi_lifecycle_pdb_invalid:{exc}")
    if tree.tag != (
        "{http://wixtoolset.org/schemas/v4/windowsinstallerdata}"
        "windowsInstallerData"
    ):
        fail("windows_msi_lifecycle_pdb_root_invalid")
    return tree


def unique_wix_action_row(
    rows: list[list[str]], action: str, table_name: str
) -> list[str]:
    matches = [row for row in rows if row and row[0] == action]
    if len(matches) != 1:
        fail(f"windows_msi_lifecycle_{table_name}_cardinality:{action}")
    return matches[0]


def parse_wix_integer(field: str, action: str, column: str) -> int:
    try:
        return int(field)
    except ValueError:
        fail(f"windows_msi_lifecycle_{column}_invalid:{action}:{field}")


def normalized_condition(value: str) -> str:
    return "".join(value.split())


def verify_windows_material_msi_lifecycle(root: Path, msi_rel: str) -> None:
    """Verify the linked MSI table model recorded in WiX's PDB.

    The PDB's wix-wid.xml records the post-link Windows Installer database,
    unlike wix-ir.json, which may still include unlinked source fragments.
    This is a read-only gate; the later Windows smoke independently verifies
    the installed group, service, configuration, and uninstall behavior.
    """

    msi_path = root / msi_rel
    pdb_path = msi_path.with_suffix(".wixpdb")
    if not pdb_path.is_file():
        fail(f"windows_msi_lifecycle_pdb_missing:{pdb_path.name}")
    tree = read_wix_linked_database(pdb_path)
    custom_actions = wix_table_rows(tree, "CustomAction")
    sequences = wix_table_rows(tree, "InstallExecuteSequence")
    admin_sequences = wix_table_rows(tree, "AdminExecuteSequence")

    for action in WINDOWS_LIFECYCLE_ACTIONS:
        row = unique_wix_action_row(custom_actions, action, "custom_action")
        if len(row) < 4:
            fail(f"windows_msi_lifecycle_custom_action_columns:{action}")
        if parse_wix_integer(row[1], action, "custom_action_type") != 11265:
            fail(f"windows_msi_lifecycle_custom_action_type:{action}")
        if row[2] != "Wix4UtilCA_X64" or row[3] != "WixQuietExec":
            fail(f"windows_msi_lifecycle_custom_action_binding:{action}")

    expected_setter_actions = {
        "SetScratchBirdPostInstall": ("ScratchBirdPostInstall", "PostInstall"),
        "SetScratchBirdPreRemove": ("ScratchBirdPreRemove", "PreRemove"),
    }
    for setter, (action_property, lifecycle_action) in expected_setter_actions.items():
        row = unique_wix_action_row(custom_actions, setter, "custom_action")
        if len(row) < 4:
            fail(f"windows_msi_lifecycle_setter_columns:{setter}")
        if parse_wix_integer(row[1], setter, "setter_type") != 51:
            fail(f"windows_msi_lifecycle_setter_type:{setter}")
        if row[2] != action_property:
            fail(f"windows_msi_lifecycle_setter_property:{setter}")
        command = row[3]
        required_command_tokens = (
            "[System64Folder]WindowsPowerShell\\v1.0\\powershell.exe",
            "powershell.exe",
            "scratchbird-windows-system-install.ps1",
            f"-Action {lifecycle_action}",
            "[INSTALLFOLDER].",
            "[CommonAppDataFolder]ScratchBird",
        )
        if any(
            token.casefold() not in command.casefold()
            for token in required_command_tokens
        ):
            fail(f"windows_msi_lifecycle_setter_command:{setter}")
        if (
            "[SystemFolder]WindowsPowerShell\\v1.0\\powershell.exe".casefold()
            in command.casefold()
        ):
            fail(f"windows_msi_lifecycle_setter_powerShell_bitness:{setter}")

    sequence_actions = (
        "InstallInitialize",
        "InstallFiles",
        "InstallFinalize",
        "RemoveFiles",
        *WINDOWS_LIFECYCLE_SETTERS,
        *WINDOWS_LIFECYCLE_ACTIONS,
    )
    sequence_rows = {
        action: unique_wix_action_row(sequences, action, "sequence")
        for action in sequence_actions
    }
    sequence_order: dict[str, int] = {}
    for action, row in sequence_rows.items():
        if len(row) < 3:
            fail(f"windows_msi_lifecycle_sequence_columns:{action}")
        sequence_order[action] = parse_wix_integer(row[2], action, "sequence")

    post_condition = normalized_condition('NOT (REMOVE~="ALL")')
    pre_condition = normalized_condition(
        'REMOVE~="ALL" AND NOT UPGRADINGPRODUCTCODE'
    )
    for action in (
        "SetScratchBirdPostInstall",
        "ScratchBirdPostInstall",
    ):
        if normalized_condition(sequence_rows[action][1]) != post_condition:
            fail(f"windows_msi_lifecycle_post_condition:{action}")
    for action in (
        "SetScratchBirdPreRemove",
        "ScratchBirdPreRemove",
    ):
        if normalized_condition(sequence_rows[action][1]) != pre_condition:
            fail(f"windows_msi_lifecycle_pre_condition:{action}")

    if not (
        sequence_order["InstallFiles"]
        < sequence_order["SetScratchBirdPostInstall"]
        < sequence_order["ScratchBirdPostInstall"]
        < sequence_order["InstallFinalize"]
    ):
        fail("windows_msi_lifecycle_post_sequence_order")
    if not (
        sequence_order["InstallInitialize"]
        < sequence_order["SetScratchBirdPreRemove"]
        < sequence_order["ScratchBirdPreRemove"]
        < sequence_order["RemoveFiles"]
    ):
        fail("windows_msi_lifecycle_pre_sequence_order")
    admin_actions = {row[0] for row in admin_sequences if row}
    if admin_actions.intersection(WINDOWS_LIFECYCLE_ACTIONS):
        fail("windows_msi_lifecycle_admin_sequence_forbidden")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--platform", choices=("linux", "windows", "macos"), required=True)
    parser.add_argument(
        "--architecture",
        choices=("x86_64", "arm64", "universal"),
        help=(
            "Target architecture for native payload admission. Linux and Windows "
            "default to x86_64; macOS is inferred only from an exact artifact-root name."
        ),
    )
    parser.add_argument(
        "--require-msi",
        action="store_true",
        help=(
            "Verify an explicitly generated Windows MSI in addition to the portable ZIP. "
            "Manual diagnostic tooling only; automated release workflows are ZIP-only."
        ),
    )
    parser.add_argument(
        "--materialize-public-root",
        type=Path,
        help=(
            "Write a clean tester-downloadable artifact root containing only "
            "the exact admitted portable package, manifest, and SHA256SUMS."
        ),
    )
    args = parser.parse_args()
    if args.require_msi and args.platform != "windows":
        fail("require_msi_platform_invalid")
    root = args.artifact_root.resolve()
    architecture = platform_architecture(args.platform, root, args.architecture)
    manifest_path = root / MANIFEST_NAME
    if not manifest_path.is_file():
        fail(f"missing_manifest:{manifest_path}")
    try:
        data = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"manifest_json_invalid:{exc}")
    if data.get("schema_id") != "scratchbird.installer_artifact_manifest.v1":
        fail("manifest_schema_mismatch")
    if data.get("platform") != args.platform:
        fail("manifest_platform_mismatch")
    artifacts = data.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts:
        fail("manifest_artifacts_missing")
    files = artifact_file_rows(root, artifacts)
    verify_manifest_checksums(root, files)
    admission, profile_digest = native_admission_call(
        native_admission.require_native_server_admission,
        data.get("native_server_admission"),
        f"installer:{root.name}",
    )
    publication_only = data.get("publication_surface") == PUBLICATION_SURFACE_SCHEMA
    if publication_only:
        require_publication_manifest(root, data, files, args.platform)
    paths = set(files)
    for suffix in REQUIRED_SUFFIXES[args.platform]:
        if not any(isinstance(path, str) and path.endswith(suffix) for path in paths):
            fail(f"required_artifact_missing:{suffix}")
    windows_msi_paths = sorted(
        path
        for path in paths
        if isinstance(path, str) and path.endswith(".msi")
    )
    windows_zip_paths = sorted(
        path
        for path in paths
        if isinstance(path, str) and path.endswith(".zip")
    )
    if args.platform == "windows":
        if len(windows_zip_paths) != 1:
            fail("windows_zip_cardinality")
        if args.require_msi:
            if not windows_msi_paths:
                fail("required_artifact_missing:.msi")
            if len(windows_msi_paths) != 1:
                fail("windows_msi_cardinality")
    # System-package evidence is an optional internal-build concern.  The
    # portable public path must neither require nor carry it.  When an
    # explicit internal build elects to emit it, still validate its authority
    # claims rather than treating the JSON as a release admission proof.
    if args.platform == "linux" and "LINUX_SYSTEM_PACKAGE_EVIDENCE.json" in paths:
        evidence = json.loads(
            (root / "LINUX_SYSTEM_PACKAGE_EVIDENCE.json").read_text(
                encoding="utf-8"
            )
        )
        identity = evidence.get("os_identity")
        if (
            not isinstance(identity, dict)
            or identity.get("service_authority_scope")
            != SERVICE_AUTHORITY_SCOPE
            or identity.get("service_effective_group_policy")
            != "only_scratchbird_effective_group"
            or identity.get("create_time_os_authorization") != "root_only"
            or identity.get("human_service_group_membership_mutation") is not False
        ):
            fail("linux_system_service_authority_scope_invalid")
    if args.platform == "windows":
        windows_block = data.get("windows")
        if not isinstance(windows_block, dict):
            fail("windows_manifest_block_missing")
        if windows_block.get("native_default_port") != 3092:
            fail("windows_native_port_mismatch")
        if args.require_msi:
            if windows_block.get("package_mode") != "portable_zip_and_msi":
                fail("windows_msi_manifest_package_mode_invalid")
            if windows_block.get("system_installer_included") is not True:
                fail("windows_msi_manifest_system_installer_missing")
            if windows_block.get("portable_archive_smoke_required") is not True:
                fail("windows_portable_archive_smoke_not_required")
            if windows_block.get("service_name") != "scratchbird":
                fail("windows_service_name_mismatch")
            if windows_block.get("service_account") != r"NT SERVICE\scratchbird":
                fail("windows_service_account_mismatch")
            if windows_block.get("actual_install_smoke_required") is not True:
                fail("windows_actual_install_smoke_not_required")
            for sidecar in WINDOWS_MSI_REQUIRED_SIDECARS:
                if sidecar not in paths:
                    fail(f"windows_sidecar_missing:{sidecar}")
            evidence = json.loads(
                (root / "WINDOWS_SYSTEM_PACKAGE_EVIDENCE.json").read_text(
                    encoding="utf-8"
                )
            )
            if (
                evidence.get("native_default_port") != 3092
                or evidence.get("database_files_created") is not False
                or evidence.get("security_sidecars_created") is not False
            ):
                fail("windows_system_evidence_invalid")
            service = evidence.get("service")
            identity = evidence.get("os_identity")
            if (
                not isinstance(service, dict)
                or service.get("name") != "scratchbird"
                or service.get("account") != r"NT SERVICE\scratchbird"
                or service.get("default_start_type") != "manual"
                or service.get("default_activity") != "stopped"
                or service.get("fresh_install_creates_missing_service") is not True
                or service.get("creation_mechanism")
                != "elevated_deferred_msi_lifecycle_helper_Ensure-SBsrvService"
                or service.get("parser_or_listener_services_installed") is not False
                or service.get("manager_service_installed") is not False
            ):
                fail("windows_system_service_evidence_invalid")
            if (
                not isinstance(identity, dict)
                or identity.get("service_authority_scope")
                != SERVICE_AUTHORITY_SCOPE
                or identity.get("local_sam_group_membership") is not False
                or identity.get("create_time_os_authorization")
                != "administrator_only"
                or identity.get("human_service_group_membership_mutation") is not False
            ):
                fail("windows_system_service_authority_scope_invalid")
            verify_windows_msi_lifecycle_wiring(root)
            pdb_rel = (
                Path(windows_msi_paths[0]).with_suffix(".wixpdb").as_posix()
            )
            if pdb_rel not in paths:
                fail(f"windows_msi_lifecycle_pdb_unmanifested:{pdb_rel}")
            verify_windows_material_msi_lifecycle(root, windows_msi_paths[0])
        else:
            if windows_msi_paths:
                fail("windows_zip_only_msi_present")
            if windows_block.get("package_mode") != "portable_zip_only":
                fail("windows_zip_only_manifest_package_mode_invalid")
            if windows_block.get("system_installer_included") is not False:
                fail("windows_zip_only_system_installer_claim")
            if windows_block.get("portable_archive_smoke_required") is not True:
                fail("windows_portable_archive_smoke_not_required")
            actual_paths = {
                path.relative_to(root).as_posix()
                for path in root.rglob("*")
                if path.is_file()
            }
            expected_paths = set(paths) | {MANIFEST_NAME, "SHA256SUMS"}
            unexpected_paths = sorted(actual_paths - expected_paths)
            if unexpected_paths:
                fail(
                    "windows_zip_only_unmanifested_artifact:"
                    + ",".join(unexpected_paths)
                )
            for rel in sorted(actual_paths):
                name = Path(rel).name
                if (
                    name in WINDOWS_ZIP_ONLY_FORBIDDEN_NAMES
                    or Path(rel).suffix.lower()
                    in WINDOWS_ZIP_ONLY_FORBIDDEN_SUFFIXES
                ):
                    fail(f"windows_zip_only_forbidden_artifact:{rel}")
    if args.platform == "macos" and not publication_only:
        macos_block = data.get("macos")
        if not isinstance(macos_block, dict):
            fail("macos_manifest_block_missing")
        support = macos_block.get("support_matrix")
        if not isinstance(support, dict):
            fail("macos_support_matrix_missing")
        for key in ("minimum_macos_version", "deployment_target", "runner_labels", "architectures", "rosetta_policy"):
            if key not in support:
                fail(f"macos_support_matrix_key_missing:{key}")
        for sidecar in MACOS_REQUIRED_SIDECARS:
            if sidecar not in paths:
                fail(f"macos_sidecar_missing:{sidecar}")
            sidecar_path = root / sidecar
            try:
                sidecar_data = json.loads(sidecar_path.read_text(encoding="utf-8"))
            except json.JSONDecodeError as exc:
                fail(f"macos_sidecar_invalid:{sidecar}:{exc}")
            if sidecar == "MACOS_DYNAMIC_LIBRARY_AUDIT.json" and not sidecar_data.get("rows"):
                fail("macos_dynamic_library_audit_empty")
            if sidecar == "MACOS_SIGNING_STATE.json":
                status = sidecar_data.get("status")
                if status not in {"qa_unsigned_not_for_public_signed_release", "payload_signed"}:
                    fail(f"macos_signing_state_invalid:{status}")
    if args.platform == "macos" and publication_only:
        status = data.get("macos_signing_status")
        if status not in {"qa_unsigned_not_for_public_signed_release", "payload_signed"}:
            fail(f"public_installer_macos_signing_status_invalid:{status}")
    # Verify every tester-downloadable payload against the manifest-bound
    # native admission. System-package formats are intentionally not accepted
    # on this public path.
    verify_native_admission_packages(
        root,
        data,
        files,
        args.platform,
        architecture,
    )
    scan(root)
    if args.materialize_public_root is not None:
        public_root = materialize_publication_root(
            root,
            args.materialize_public_root.resolve(),
            data,
            files,
            args.platform,
        )
        public_manifest_path = public_root / MANIFEST_NAME
        public_data = json.loads(public_manifest_path.read_text(encoding="utf-8"))
        public_rows = public_data.get("artifacts")
        if not isinstance(public_rows, list):
            fail("public_installer_manifest_artifacts_missing")
        public_files = artifact_file_rows(public_root, public_rows)
        verify_manifest_checksums(public_root, public_files)
        require_publication_manifest(
            public_root, public_data, public_files, args.platform
        )
        public_admission, public_profile_digest = native_admission_call(
            native_admission.require_native_server_admission,
            public_data.get("native_server_admission"),
            f"public_installer:{public_root.name}",
        )
        del public_admission
        verify_native_admission_packages(
            public_root,
            public_data,
            public_files,
            args.platform,
            architecture,
        )
        if public_profile_digest != profile_digest:
            fail("public_installer_profile_digest_mismatch")
    print(f"verify_installer_artifacts=passed:{root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
