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
import sys
import zipfile
import xml.etree.ElementTree as ET


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
    "linux": (".tar.gz", ".deb"),
    "windows": (".zip",),
    "macos": (".tar.gz", ".pkg"),
}

MACOS_REQUIRED_SIDECARS = (
    "MACOS_DYNAMIC_LIBRARY_AUDIT.json",
    "MACOS_SIGNING_STATE.json",
    "MACOS_SYSTEM_PACKAGE_EVIDENCE.json",
)
WINDOWS_REQUIRED_SIDECARS = (
    "WINDOWS_SYSTEM_PACKAGE_EVIDENCE.json",
    "scratchbird.wxs",
    "scratchbird-windows-lifecycle.wxs",
)
LINUX_REQUIRED_SIDECARS = ("LINUX_SYSTEM_PACKAGE_EVIDENCE.json",)
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
        "--require-msi",
        action="store_true",
        help="Require a material MSI in addition to the portable Windows ZIP.",
    )
    args = parser.parse_args()
    root = args.artifact_root.resolve()
    manifest_path = root / MANIFEST_NAME
    if not manifest_path.is_file():
        fail(f"missing_manifest:{manifest_path}")
    data = json.loads(manifest_path.read_text(encoding="utf-8"))
    if data.get("schema_id") != "scratchbird.installer_artifact_manifest.v1":
        fail("manifest_schema_mismatch")
    if data.get("platform") != args.platform:
        fail("manifest_platform_mismatch")
    artifacts = data.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts:
        fail("manifest_artifacts_missing")
    paths = {row.get("path") for row in artifacts if isinstance(row, dict)}
    for suffix in REQUIRED_SUFFIXES[args.platform]:
        if not any(isinstance(path, str) and path.endswith(suffix) for path in paths):
            fail(f"required_artifact_missing:{suffix}")
    windows_msi_paths = sorted(
        path
        for path in paths
        if isinstance(path, str) and path.endswith(".msi")
    )
    if args.platform == "windows" and args.require_msi:
        if not windows_msi_paths:
            fail("required_artifact_missing:.msi")
        if len(windows_msi_paths) != 1:
            fail("windows_msi_cardinality")
    for row in artifacts:
        if not isinstance(row, dict):
            fail("manifest_row_not_object")
        rel = row.get("path")
        if not isinstance(rel, str):
            fail("manifest_row_path_invalid")
        path = root / rel
        if not path.is_file():
            fail(f"manifest_file_missing:{rel}")
        if row.get("bytes") != path.stat().st_size:
            fail(f"manifest_size_mismatch:{rel}")
        if row.get("sha256") != sha256_file(path):
            fail(f"manifest_sha256_mismatch:{rel}")
    if args.platform == "linux":
        for sidecar in LINUX_REQUIRED_SIDECARS:
            if sidecar not in paths:
                fail(f"linux_sidecar_missing:{sidecar}")
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
        if windows_block.get("service_name") != "scratchbird":
            fail("windows_service_name_mismatch")
        if windows_block.get("service_account") != r"NT SERVICE\scratchbird":
            fail("windows_service_account_mismatch")
        if windows_block.get("native_default_port") != 3092:
            fail("windows_native_port_mismatch")
        if windows_block.get("actual_install_smoke_required") is not True:
            fail("windows_actual_install_smoke_not_required")
        for sidecar in WINDOWS_REQUIRED_SIDECARS:
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
        if args.require_msi:
            pdb_rel = (
                Path(windows_msi_paths[0]).with_suffix(".wixpdb").as_posix()
            )
            if pdb_rel not in paths:
                fail(f"windows_msi_lifecycle_pdb_unmanifested:{pdb_rel}")
            verify_windows_material_msi_lifecycle(root, windows_msi_paths[0])
    if args.platform == "macos":
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
            if sidecar == "MACOS_SYSTEM_PACKAGE_EVIDENCE.json":
                if sidecar_data.get("schema_id") != (
                    "scratchbird.macos_system_package_evidence.v1"
                ):
                    fail("macos_system_package_evidence_schema_mismatch")
                if sidecar_data.get("native_default_port") != 3092:
                    fail("macos_system_package_evidence_native_port_mismatch")
                if sidecar_data.get("database_files_created") is not False:
                    fail("macos_system_package_evidence_database_creation")
                if sidecar_data.get("security_sidecars_created") is not False:
                    fail("macos_system_package_evidence_security_sidecar_creation")
                identity = sidecar_data.get("os_identity")
                if (
                    not isinstance(identity, dict)
                    or identity.get("service_authority_scope")
                    != SERVICE_AUTHORITY_SCOPE
                    or identity.get("create_time_os_authorization") != "root_only"
                    or identity.get("human_service_group_membership_mutation") is not False
                    or identity.get("resolved_effective_group_policy")
                    != (
                        "launchd_host_computed_groups_cleared_before_"
                        "scratchbird_product_exec"
                    )
                ):
                    fail("macos_system_service_authority_scope_invalid")
                service = sidecar_data.get("service")
                if (
                    not isinstance(service, dict)
                    or service.get("launchd_init_groups") is not False
                    or service.get("launchd_bootstrap_identity") != "root:wheel"
                    or service.get("service_launcher")
                    != "/opt/ScratchBird/bin/SBlaunch"
                    or service.get("service_launcher_interface")
                    != "fixed_selector_only_no_forwarded_arguments"
                    or service.get("final_product_identity")
                    != "scratchbird:scratchbird"
                    or service.get("final_supplementary_groups") != []
                ):
                    fail("macos_system_launcher_contract_invalid")
    scan(root)
    print(f"verify_installer_artifacts=passed:{root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
