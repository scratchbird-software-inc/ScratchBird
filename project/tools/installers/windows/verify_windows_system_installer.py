#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Linux-runnable gate for native Windows system-installer assets."""

from __future__ import annotations

import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parent
PROFILE = ROOT / "WINDOWS_SYSTEM_INSTALL_PROFILE.json"
LIFECYCLE = ROOT / "scratchbird-windows-system-install.ps1"
WIX = ROOT / "scratchbird-windows-lifecycle.wxs.in"
SMOKE = ROOT.parent / "smoke_install_windows.ps1"
BUILDER = ROOT.parent / "build_installers.py"
ARTIFACT_VERIFIER = ROOT.parent / "verify_installer_artifacts.py"
WORKFLOW = ROOT.parents[3] / ".github" / "workflows" / "verify-installers.yml"


def fail(code: str) -> None:
    print(f"verify_windows_system_installer=fail:{code}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, code: str) -> None:
    if not condition:
        fail(code)


def require_tokens(text: str, tokens: tuple[str, ...], prefix: str) -> None:
    for token in tokens:
        require(token in text, f"{prefix}_token_missing:{token}")


def main() -> int:
    for path in (
        PROFILE,
        LIFECYCLE,
        WIX,
        SMOKE,
        BUILDER,
        ARTIFACT_VERIFIER,
        WORKFLOW,
    ):
        require(path.is_file(), f"asset_missing:{path.name}")

    profile_text = PROFILE.read_text(encoding="utf-8")
    profile = json.loads(profile_text)
    lifecycle = LIFECYCLE.read_text(encoding="utf-8")
    wix = WIX.read_text(encoding="utf-8")
    smoke = SMOKE.read_text(encoding="utf-8")
    builder = BUILDER.read_text(encoding="utf-8")
    artifact_verifier = ARTIFACT_VERIFIER.read_text(encoding="utf-8")
    workflow = WORKFLOW.read_text(encoding="utf-8")
    combined = profile_text + lifecycle + wix

    require(
        profile.get("schema_id")
        == "scratchbird.windows_system_install_profile.v1",
        "profile_schema",
    )
    require(
        profile.get("distribution_profile") == "native-sbsql-only",
        "distribution_profile",
    )
    require(profile.get("native_default_port") == 3092, "native_default_port")
    for alternate in ("3050", "3090", "3392"):
        require(alternate not in combined, f"alternate_native_port:{alternate}")
    require(profile.get("database_files_created") is False, "database_policy")
    require(
        profile.get("security_sidecars_created") is False,
        "security_sidecar_policy",
    )

    service = profile.get("service", {})
    require(service.get("name") == "scratchbird", "service_name")
    require(
        service.get("default_start_type") == "manual",
        "service_start_type",
    )
    require(
        service.get("default_activity") == "stopped",
        "service_activity",
    )
    require(
        service.get("service_sid_type") == "restricted",
        "service_sid_type",
    )
    require(
        service.get("create_if_missing") is True,
        "service_create_if_missing",
    )
    require(
        service.get("creation_mechanism")
        == "elevated_deferred_msi_lifecycle_helper_Ensure-SBsrvService",
        "service_creation_mechanism",
    )
    require(
        service.get("fresh_install_failure_service_rollback")
        == "remove_service_created_by_this_install_attempt",
        "service_creation_rollback",
    )
    require(service.get("top_level_process") == "SBsrv", "service_top_level")
    require("--service" in service.get("arguments", []), "service_mode")
    require(
        service.get("scm_runtime_handoff")
        == "native_SBsrv_ServiceMain_in_process_no_wrapper",
        "scm_runtime_handoff",
    )
    require(
        service.get("scm_running_gate")
        == "SBPS_and_shared_SBgate_management_ready",
        "scm_running_gate",
    )
    require(
        service.get("scm_stop_path")
        == "in_process_parser_server_stop_api",
        "scm_stop_path",
    )
    require(
        service.get("scm_validation_state")
        == "focused_contract_green_hosted_configured_database_start_stop_pending",
        "scm_validation_state",
    )

    topology = profile.get("topology", {})
    require(
        topology.get("native_route")
        == (
            "client_to_optional_SBmgr_not_used_with_emulation_to_shared_"
            "SBgate_to_standalone_selected_SBParser_to_SBPS_IPC_to_"
            "SBsrv_engine"
        ),
        "native_topology",
    )
    require(
        topology.get("listener_model")
        == "one_shared_listener_executable_for_all_parser_families",
        "shared_listener_model",
    )
    require(
        topology.get("parser_process_model")
        == "one_standalone_parser_for_the_selected_dialect",
        "standalone_selected_parser_model",
    )
    require(
        topology.get("parser_engine_transport") == "sbps_ipc_only",
        "parser_engine_transport",
    )
    require(
        topology.get("direct_engine_link") == "forbidden"
        and topology.get("cross_parser_dependency") == "forbidden",
        "standalone_parser_boundaries",
    )
    require(
        topology.get("parser_specific_services") is False,
        "parser_specific_service",
    )
    require(
        all(
            token in topology.get("manager", "")
            for token in (
                "optional_SBmgr",
                "disabled_by_default",
                "bypassed_not_used_with_emulation",
                "not_installed_as_a_service",
            )
        ),
        "manager_service_policy",
    )
    require(
        profile.get("shipped_native_components", []).count("SBmgr") == 1,
        "manager_not_shipped",
    )

    identity = profile.get("os_identity", {})
    require(identity.get("group") == "ScratchBird", "group_name")
    require(identity.get("group_namespace") == "local_SAM", "group_namespace")
    require(
        identity.get("group_principal") == "exact_local_SAM_alias",
        "group_type_validation",
    )
    require(
        identity.get("service_account") == r"NT SERVICE\scratchbird",
        "service_account",
    )
    require(
        identity.get("service_account_leaf_name") == "scratchbird",
        "service_leaf",
    )
    require(
        identity.get("service_account_namespace") == "NT SERVICE",
        "service_namespace",
    )
    require(
        identity.get("service_account_kind")
        == "restricted_managed_virtual_service_account",
        "service_account_kind",
    )
    require(
        identity.get("service_authority_scope")
        == (
            "filesystem_directory_and_process_execution_only_"
            "no_database_or_security_authority"
        ),
        "service_authority_scope",
    )
    require(identity.get("service_password") == "none", "service_password")
    require(
        identity.get("service_sam_user_created") is False,
        "service_sam_user_created",
    )
    require(
        identity.get("service_admin_group_membership")
        == "forbidden_and_verified_absent",
        "service_admin_membership",
    )
    require(
        identity.get("service_local_sam_group_membership")
        == "forbidden_and_verified_absent_across_all_local_groups",
        "service_local_sam_group_membership",
    )
    require(
        "no_password_or_SAM_logon_principal"
        in identity.get("interactive_remote_network_batch_credentials", ""),
        "service_interactive_logon_fence",
    )
    require(
        "case_insensitive" in identity.get("naming_constraint", ""),
        "windows_naming_constraint",
    )

    require(
        profile.get("create_time_os_authorization") == "administrator_only",
        "create_time_os_authorization",
    )
    require(
        profile.get("human_service_group_membership_mutation") == "forbidden",
        "human_service_group_membership_mutation",
    )
    require(
        identity.get("group_purpose")
        == "filesystem_operations_only_no_database_or_security_authority",
        "filesystem_operations_group_purpose",
    )

    require(
        profile.get("upgrade_policy", {}).get("silent_activation") is False,
        "upgrade_silent_activation",
    )
    require(
        profile.get("uninstall_policy", {}).get("configuration") == "preserve",
        "uninstall_config",
    )
    require(
        profile.get("uninstall_policy", {}).get("data") == "preserve",
        "uninstall_data",
    )

    required_dirs = {
        r"%ProgramData%\ScratchBird\config",
        r"%ProgramData%\ScratchBird\data",
        r"%ProgramData%\ScratchBird\log",
        r"%ProgramData%\ScratchBird\run",
        r"%ProgramData%\ScratchBird\run\control",
        r"%ProgramData%\ScratchBird\tls",
        r"%ProgramData%\ScratchBird\secrets",
    }
    actual_dirs = {row.get("path") for row in profile.get("directories", [])}
    require(required_dirs.issubset(actual_dirs), "protected_directories")

    require_tokens(
        lifecycle,
        (
            "Win32_Group",
            "LocalAccount=TRUE",
            "[int]$row.SIDType -ne 4",
            "NT SERVICE\\scratchbird",
            '"start= demand"',
            '@("sidtype", $ServiceName, "restricted")',
            "ServiceSidType",
            "SetAccessRuleProtection($true, $false)",
            "Grant-ServiceRuntimeReadExecute",
            "Set-ProtectedServiceRegistryAcl",
            "Copy-MissingConfigurationDefaults",
            '@SCRATCHBIRD_INSTALL_ROOT@',
            '@SCRATCHBIRD_STATE_ROOT@',
            "human_service_group_membership_mutated = $false",
            'create_time_os_authorization = "administrator_only"',
            "Get-InstallerDatabaseArtifactInventory",
            "Assert-DatabaseArtifactInventoryUnchanged",
            "client_to_optional_SBmgr_not_used_with_emulation_to_shared_SBgate_to_standalone_selected_SBParser_to_SBPS_IPC_to_SBsrv_engine",
            "Name='scratchbird' AND LocalAccount=TRUE",
            'Win32_Group -Filter "LocalAccount=TRUE"',
            "service_local_sam_group_membership = $false",
            "filesystem_directory_and_process_execution_only_no_database_or_security_authority",
            "$ServiceCreatedByThisRun",
            "function Rollback-CreatedService",
            "Rollback-CreatedService",
            "function Ensure-SBsrvService",
            '@("create", $ServiceName',
            "Assert-ServiceRecord $service -RequireFreshDefaults",
            "& $sc delete $ServiceName",
        ),
        "lifecycle",
    )
    for forbidden in (
        "Start-Service",
        "Set-Service -StartupType Automatic",
        "New-Service -Name SBgate",
        "New-Service -Name SBParser",
        "New-Service -Name SBmgr",
        "CREATE DATABASE",
        "New-Item -ItemType File",
        "InstallerUser",
        "GROUP_MEMBERSHIP_REQUIRED",
        ".Add((\"WinNT://",
    ):
        require(forbidden not in lifecycle, f"lifecycle_forbidden:{forbidden}")

    require(
        lifecycle.count('"create", $ServiceName') == 1,
        "multiple_service_create_paths",
    )
    require(
        "$env:USERNAME" not in lifecycle
        and "$env:USERDOMAIN" not in lifecycle,
        "ambient_installer_user",
    )
    require(
        "New-LocalUser" not in lifecycle and "net user" not in lifecycle,
        "local_password_account_created",
    )

    try:
        tree = ET.fromstring(wix)
    except ET.ParseError as exc:
        fail(f"wix_xml:{exc}")
    ns = {"w": "http://wixtoolset.org/schemas/v4/wxs"}
    properties = tree.findall(".//w:Property", ns)
    require(
        all(row.get("Id") != "SB_INSTALLER_USER" for row in properties),
        "wix_installer_user_property_forbidden",
    )
    registry_values = tree.findall(".//w:RegistryValue", ns)
    require(
        all(row.get("Name") != "InstallerUser" for row in registry_values),
        "wix_installer_user_registry_staging_forbidden",
    )
    actions = {
        row.get("Id"): row
        for row in tree.findall(".//w:CustomAction", ns)
    }
    for action_id in ("ScratchBirdPostInstall", "ScratchBirdPreRemove"):
        require(action_id in actions, f"wix_action_missing:{action_id}")
        require(
            actions[action_id].get("Execute") == "deferred",
            f"wix_action_not_deferred:{action_id}",
        )
        require(
            actions[action_id].get("Impersonate") == "no",
            f"wix_action_impersonation:{action_id}",
        )
        require(
            actions[action_id].get("HideTarget") == "yes",
            f"wix_action_target_visible:{action_id}",
        )
    set_properties = {
        row.get("Id"): row.get("Value", "")
        for row in tree.findall(".//w:SetProperty", ns)
    }
    for action_id in ("ScratchBirdPostInstall", "ScratchBirdPreRemove"):
        require(
            '-InstallRoot "[INSTALLFOLDER]."'
            in set_properties.get(action_id, ""),
            f"wix_install_root_trailing_separator_guard:{action_id}",
        )
        require(
            '"[System64Folder]WindowsPowerShell\\v1.0\\powershell.exe"'
            in set_properties.get(action_id, ""),
            f"wix_lifecycle_64bit_powershell_required:{action_id}",
        )
        require(
            '"[SystemFolder]WindowsPowerShell\\v1.0\\powershell.exe"'
            not in set_properties.get(action_id, ""),
            f"wix_lifecycle_32bit_powershell_forbidden:{action_id}",
        )
    require(
        "REMOVE~=&quot;ALL&quot; AND NOT UPGRADINGPRODUCTCODE" in wix,
        "wix_upgrade_remove_guard",
    )
    require(
        "SB_INSTALLER_USER" not in wix and "InstallerUser" not in wix,
        "wix_installer_user_surface_forbidden",
    )
    require(
        wix.count("<Fragment") == 1,
        "wix_lifecycle_fragment_split",
    )

    require_tokens(
        smoke,
        (
            '"/a"',
            '"/i"',
            '"/x"',
            "$AdministrativeExtractOnly",
            "passed_separate_no_lifecycle_claim",
            "qa-operator-preserve.conf",
            "qa-operator-preserve.dat",
            "Assert-SidNotInAnyLocalSamGroup",
            "service_local_sam_group_membership = $false",
            "human_service_group_membership_mutated = $false",
            'create_time_os_authorization = "administrator_only"',
            "filesystem_directory_and_process_execution_only_no_database_or_security_authority",
            "NT SERVICE\\scratchbird",
            "service_fresh_install = \"manual_stopped\"",
            "scm_runtime_start_proof = \"not_claimed_by_installer_lifecycle_smoke\"",
        ),
        "smoke",
    )
    require(
        "$env:USERNAME" not in smoke and "$env:USERDOMAIN" not in smoke,
        "smoke_ambient_installer_user",
    )
    require(
        smoke.index('"/a"') < smoke.index('"/i"'),
        "administrative_extract_not_separate",
    )
    administrative_root_limit = re.search(
        r"(?m)^\$MaximumMsiAdministrativeExtractRootLength\s*=\s*(\d+)\s*$",
        smoke,
    )
    require(
        administrative_root_limit is not None
        and int(administrative_root_limit.group(1)) <= 40,
        "administrative_extract_root_length_limit",
    )
    require_tokens(
        smoke,
        (
            "function New-ShortMsiAdministrativeExtractionRoot",
            "$env:RUNNER_TEMP",
            "[System.Environment+SpecialFolder]::UserProfile",
            "$payloadRoot = New-ShortMsiAdministrativeExtractionRoot",
            'TARGETDIR=`"$payloadRoot`"',
            'Get-ChildItem -Path $payloadRoot -Recurse -Filter "NATIVE_RELEASE_PROFILE.json" -File',
            "$nativeProfiles.Count -ne 1",
            "$runtimeRoot = $nativeProfile.Directory.Parent.Parent.Parent",
            '$portableConfigRoot = Join-Path $runtimeRoot.FullName "etc\\scratchbird"',
            '$configDefaultsRoot = Join-Path $runtimeRoot.FullName "share\\scratchbird\\config-defaults"',
            "--config-root $configDefaultsRoot",
            "MSI administrative image retains portable etc config tree",
            "Remove-Item -LiteralPath $administrativeExtractRoot -Recurse -Force",
        ),
        "smoke_administrative_extract_path",
    )
    require(
        '$payloadRoot = Join-Path $WorkRoot "administrative-extract"' not in smoke,
        "administrative_extract_deep_work_root",
    )
    require(
        "CommonDocuments" not in smoke
        and "[IO.Path]::GetPathRoot($WorkRoot)" not in smoke,
        "administrative_extract_shared_root_forbidden",
    )

    require_tokens(
        builder,
        (
            "def stage_windows_system_install_tree(",
            "def write_windows_system_install_profile(",
            "def write_windows_system_package_evidence(",
            "def materialize_windows_wix_lifecycle(",
            '<CustomActionRef Id="ScratchBirdPostInstall" />',
            '<CustomActionRef Id="ScratchBirdPreRemove" />',
            'msi.with_suffix(".wixpdb").is_file()',
            "windows_wix_pdb_missing",
            "WINDOWS_NATIVE_CONFIGS = (",
            '"SBbootstrap.profile"',
            '"windows-system-payload"',
            "stage_windows_system_install_tree(",
            "write_windows_system_package_evidence(",
            '"WixToolset.Util.wixext"',
            '"native_default_port": 3092',
            r'service_identity = NT SERVICE\scratchbird',
            "@SCRATCHBIRD_STATE_ROOT@",
            "@SCRATCHBIRD_INSTALL_ROOT@",
        ),
        "builder",
    )
    require_tokens(
        artifact_verifier,
        (
            "zipfile.ZipFile",
            'WIX_PDB_DATA_ENTRY = "wix-wid.xml"',
            "def verify_windows_material_msi_lifecycle(",
            "ScratchBirdPostInstall",
            "ScratchBirdPreRemove",
            "SetScratchBirdPostInstall",
            "SetScratchBirdPreRemove",
            "System64Folder",
            "windows_msi_lifecycle_setter_powerShell_bitness",
            "windows_msi_lifecycle_pdb_unmanifested",
            "windows_msi_lifecycle_post_sequence_order",
            "windows_msi_lifecycle_pre_sequence_order",
            "windows_msi_lifecycle_admin_sequence_forbidden",
        ),
        "artifact_lifecycle",
    )
    require_tokens(
        workflow,
        (
            "Set up WiX and utility extension",
            "wix extension add -g",
            "dotnet tool install --global wix --version 4.0.6",
            "WixToolset.Util.wixext/4.0.6",
            "--require-msi",
            'throw "No Windows MSI package found"',
        ),
        "workflow",
    )
    windows_job_marker = "  windows-installers:\n"
    windows_job_start = workflow.find(windows_job_marker)
    require(windows_job_start >= 0, "workflow_windows_installers_job_missing")
    build_installers_marker = "      - name: Build installers\n"
    build_installers_start = workflow.find(
        build_installers_marker,
        windows_job_start + len(windows_job_marker),
    )
    require(build_installers_start >= 0, "workflow_build_installers_step_missing")
    build_installers_end = workflow.find(
        "\n      - name:", build_installers_start + len(build_installers_marker)
    )
    if build_installers_end < 0:
        build_installers_end = len(workflow)
    build_installers_step = workflow[build_installers_start:build_installers_end]
    wix_visibility_tokens = (
        'wix_tool_root="$(cygpath -u "$SB_WIX_TOOL_ROOT")"',
        'wix_global_tool="$wix_tool_root/wix.exe"',
        'test -f "$wix_global_tool"',
        '"$wix_global_tool" --version',
        'export PATH="$(dirname "$wix_global_tool"):$PATH"',
        "command -v wix >/dev/null",
    )
    require_tokens(
        build_installers_step,
        wix_visibility_tokens,
        "workflow_wix_global_tool_visibility",
    )
    require(
        '"SB_WIX_TOOL_ROOT=$toolRoot" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append'
        in workflow,
        "workflow_wix_tool_root_environment_missing",
    )
    builder_command = "python project/tools/installers/build_installers.py"
    require(
        builder_command in build_installers_step,
        "workflow_wix_builder_command_missing",
    )
    builder_command_offset = build_installers_step.index(builder_command)
    for token in wix_visibility_tokens:
        require(
            build_installers_step.index(token) < builder_command_offset,
            f"workflow_wix_visibility_after_builder:{token}",
        )
    require(
        "--require-msi" in build_installers_step,
        "workflow_wix_msi_requirement_missing",
    )
    require_tokens(
        builder,
        (
            'wix_bin = shutil.which("wix")',
            "if require_msi:",
            'fail("wix_not_found")',
        ),
        "builder_wix_fail_closed",
    )
    require(
        'Filter "scratchbird-windows-*.zip"' not in workflow,
        "workflow_msi_fallback_present",
    )

    # Database-looking names may appear only in the negative artifact scan.
    for match in re.finditer(
        r"(?i)(sbdb|security_principal_events|local_password_auth)",
        lifecycle,
    ):
        context = lifecycle[
            max(0, match.start() - 100) : match.end() + 100
        ]
        require(
            "WriteAllText" not in context and "Set-Content" not in context,
            "database_sidecar_write_path",
        )

    print("verify_windows_system_installer=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
