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
    "scratchbird-windows-lifecycle.wxs",
)
LINUX_REQUIRED_SIDECARS = ("LINUX_SYSTEM_PACKAGE_EVIDENCE.json",)
SERVICE_AUTHORITY_SCOPE = (
    "filesystem_directory_and_process_execution_only_"
    "no_database_or_security_authority"
)
MACOS_SERVICE_PROCESS_GROUP_POLICY = (
    "launchd_host_computed_groups_cleared_before_scratchbird_product_exec"
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
    if args.platform == "windows" and args.require_msi:
        if not any(
            isinstance(path, str) and path.endswith(".msi") for path in paths
        ):
            fail("required_artifact_missing:.msi")
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
            or identity.get("filesystem_operations_group_creation_mechanism")
            != r"Microsoft.PowerShell.LocalAccounts\New-LocalGroup"
            or identity.get("lifecycle_process_architecture")
            != "64_bit_required"
            or identity.get("create_time_os_authorization")
            != "administrator_only"
            or identity.get("human_service_group_membership_mutation") is not False
        ):
            fail("windows_system_service_authority_scope_invalid")
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
                if sidecar_data.get("service", {}).get("launchd_init_groups") is not False:
                    fail("macos_system_launchd_init_groups_not_disabled")
                service = sidecar_data.get("service")
                if (
                    not isinstance(service, dict)
                    or service.get("launchd_bootstrap_identity") != "root:wheel"
                    or service.get("launchd_definition_path_policy")
                    != "root_owned_0644_no_extended_acl_no_symlink_single_link_exact_fixed_selector"
                    or service.get("launchd_standard_log_root")
                    != "/var/log/scratchbird/launchd"
                    or service.get("launchd_standard_log_file_identity")
                    != "root:scratchbird:0640_no_extended_acl"
                    or service.get("service_launcher")
                    != "/opt/ScratchBird/bin/SBlaunch"
                    or service.get("service_launcher_interface")
                    != "fixed_selector_only_no_forwarded_arguments"
                    or service.get("service_launcher_path_policy")
                    != "root_owned_nonwritable_no_extended_acl_no_symlink_single_link_launcher"
                    or service.get("final_product_identity")
                    != "scratchbird:scratchbird"
                    or service.get("final_supplementary_groups") != []
                    or service.get("service_runtime_log_root")
                    != "/var/log/scratchbird/runtime"
                    or service.get("loaded_legacy_launchd_job_upgrade_policy")
                    != "reject_before_payload_replacement_and_recheck_postinstall"
                    or service.get("package_preinstall_existing_topology_policy")
                    != "reject_unsafe_existing_root_helper_launcher_and_plist_paths_before_payload"
                    or service.get("package_postinstall_helper_path_policy")
                    != "pre_exec_root_owned_0755_no_extended_acl_no_symlink_single_link_helper"
                    or service.get("legacy_packaged_log_default_migration_policy")
                    != "exact_prior_packaged_line_only_preserve_all_other_configuration_lines"
                ):
                    fail("macos_system_service_launcher_evidence_invalid")
                identity = sidecar_data.get("os_identity")
                if (
                    not isinstance(identity, dict)
                    or identity.get("service_authority_scope")
                    != SERVICE_AUTHORITY_SCOPE
                    or identity.get("create_time_os_authorization") != "root_only"
                    or identity.get("human_service_group_membership_mutation") is not False
                    or identity.get("password_record") != "literal_asterisk_lock"
                    or identity.get("authentication_authority") != "absent"
                    or identity.get("shadow_hash_data") != "absent"
                    or identity.get("group_membership_policy")
                    != "exact_scratchbird_name_and_generated_uid_only_no_nested_groups"
                    or identity.get("resolved_effective_group_policy")
                    != MACOS_SERVICE_PROCESS_GROUP_POLICY
                ):
                    fail("macos_system_service_authority_scope_invalid")
    scan(root)
    print(f"verify_installer_artifacts=passed:{root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
