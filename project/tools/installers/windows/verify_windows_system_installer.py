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
    for path in (PROFILE, LIFECYCLE, WIX, SMOKE, BUILDER, WORKFLOW):
        require(path.is_file(), f"asset_missing:{path.name}")

    profile_text = PROFILE.read_text(encoding="utf-8")
    profile = json.loads(profile_text)
    lifecycle = LIFECYCLE.read_text(encoding="utf-8")
    wix = WIX.read_text(encoding="utf-8")
    smoke = SMOKE.read_text(encoding="utf-8")
    builder = BUILDER.read_text(encoding="utf-8")
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
    transaction = profile.get("installer_transaction", {})
    require(
        transaction.get("rollback_required") is True,
        "installer_transaction_rollback_required",
    )
    require(
        transaction.get("rollback_disabled_policy")
        == "blocked_by_package_launch_condition",
        "installer_transaction_rollback_disabled_policy",
    )
    require(
        transaction.get("journal")
        == r"HKLM\SOFTWARE\ScratchBird\InstallerTransaction",
        "installer_transaction_journal",
    )
    require(
        transaction.get("rollback_scope")
        == "service_and_filesystem_operations_group_identity_only",
        "installer_transaction_scope",
    )
    require(
        transaction.get("fresh_install_failure")
        == "remove_service_and_group_created_by_install_attempt",
        "installer_transaction_fresh_install_failure",
    )
    require(
        transaction.get("uninstall_failure")
        == (
            "restore_snapshotted_service_identity_configuration_and_"
            "runtime_state_fields_and_verify_preserved_group_identity"
        ),
        "installer_transaction_uninstall_failure",
    )
    require(
        transaction.get("programdata_configuration_and_acl_policy")
        == "preserved_not_rolled_back_and_required_acl_reapplied_on_retry",
        "installer_transaction_programdata_policy",
    )
    require(
        transaction.get("post_install_identity_finalization")
        == "checked_deferred_before_install_finalize",
        "installer_transaction_post_install_finalization",
    )
    require(
        transaction.get("post_install_journal_cleanup")
        == (
            "ignored_commit_after_successful_install_finalize_"
            "fixed_absolute_System32_reg.exe_exact_key_delete"
        ),
        "installer_transaction_post_install_cleanup",
    )
    require(
        transaction.get("pre_remove_journal_cleanup")
        == (
            "ignored_commit_after_successful_install_finalize_"
            "fixed_absolute_System32_reg.exe_exact_key_delete"
        ),
        "installer_transaction_pre_remove_cleanup",
    )
    require(
        transaction.get("fault_injection") == "WIXFAILWHENDEFERRED=1",
        "installer_transaction_fault_injection",
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
        identity.get("group_creation_mechanism")
        == "absolute_System32_net.exe_localgroup_add",
        "group_creation_mechanism",
    )
    require(
        identity.get("group_creation_process_architecture")
        == "64_bit_required",
        "group_creation_process_architecture",
    )
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
        identity.get("group_membership_policy")
        == "must_be_empty_no_human_or_service_members",
        "filesystem_operations_group_membership_policy",
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
            '"sidtype",',
            '"restricted"',
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
            '$group.PSBase.Invoke("Members")',
            "if ($members.Count -ne 0)",
            "filesystem_operations_group_member_count = 0",
            'filesystem_operations_group_creation_policy = "absolute_System32_net.exe_localgroup_add_when_missing"',
            "filesystem_operations_group_created_by_this_run = [bool]$GroupCreatedByThisRun",
            'lifecycle_process_architecture = "64_bit"',
            "service_local_sam_group_membership = $false",
            "filesystem_directory_and_process_execution_only_no_database_or_security_authority",
            "function Ensure-SBsrvService",
            '$LifecyclePhase = "PRECHECK"',
            "[Environment]::Is64BitProcess",
            '$GroupName = "ScratchBird"',
            '$GroupDescription = "ScratchBird filesystem operations group; no database or security authority"',
            "function Get-SystemNetExecutable",
            "$systemDirectory = [Environment]::SystemDirectory",
            '$candidate = Join-Path $canonicalSystemDirectory "net.exe"',
            "[IO.FileAttributes]::ReparsePoint",
            "[IO.Path]::GetDirectoryName($canonicalCandidate)",
            '[IO.Path]::GetFileName($canonicalCandidate)',
            "function Get-PostInstallGroupComment",
            '$script:LifecyclePhase = "GROUP_IDENTITY_NATIVE_PATH"',
            '$script:LifecyclePhase = "GROUP_IDENTITY_CREATE"',
            '& $net "localgroup" $GroupName "/add" '
            '"/comment:$transactionComment" 1>$null 2>$null',
            "$nativeStatus = [int]$LASTEXITCODE",
            "if ($nativeStatus -ne 0)",
            "[Globalization.CultureInfo]::InvariantCulture",
            '$script:LifecyclePhase = "GROUP_IDENTITY_CREATE_EXIT_$nativeStatusText"',
            '$script:LifecyclePhase = "GROUP_IDENTITY_POSTFAILURE_INVENTORY"',
            '$script:LifecyclePhase = "GROUP_IDENTITY_FINAL_VALIDATE"',
            '"BOOTSTRAP.GROUP_CREATE_FAILED.$creationFailurePhase"',
            '"BOOTSTRAP.INSTALL_DEFAULTS_INVALID.$LifecyclePhase"',
            '"create",',
            "Assert-ServiceRecord $service -RequireFreshDefaults",
            'Invoke-NativeQuiet $sc @("delete", $ServiceName)',
            '$TransactionKey = "HKLM:\\SOFTWARE\\ScratchBird\\InstallerTransaction"',
            '$TransactionStateName = "State"',
            '$TransactionSchema = "scratchbird.windows_installer_transaction.v1"',
            '"PostInstall"',
            '"RollbackPostInstall"',
            '"CommitPostInstall"',
            '"PreRemove"',
            '"RollbackPreRemove"',
            "function New-TransactionRegistryAcl",
            "function Initialize-TransactionState",
            "function Write-TransactionState",
            "function Read-TransactionState",
            "function Assert-TransactionStateShape",
            "function Remove-TransactionState",
            "function New-PostInstallTransactionState",
            "function New-PreRemoveTransactionState",
            "function Invoke-RollbackPostInstall",
            "function Invoke-CommitPostInstall",
            "function Invoke-PreRemove",
            "function Invoke-RollbackPreRemove",
            "function Restore-PreRemoveService",
            "function Test-ServiceMatchesSnapshot",
            'operation = "post_install"',
            'operation = "pre_remove"',
            'existing_configuration = "preserve_never_overwrite"',
            'existing_state_directory_acls = "preserve_on_identity_rollback"',
            'filesystem_operations_group = "preserve_never_delete"',
            'service_snapshot = "restore_only_when_exact_service_remains_absent"',
            "service_security_sddl",
            "registry_security_sddl",
            "delayed_auto_start_present",
            "commit_completed = $false",
            "$transaction.commit_completed = $true",
            "Remove-TransactionState $transaction",
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
        "$_.Exception",
        '$computer.Create("group", $GroupName)',
        "$group.SetInfo()",
        "Microsoft.PowerShell.LocalAccounts",
        "New-LocalGroup",
        "$ServiceCreatedByThisRun",
        "function Rollback-CreatedService",
    ):
        require(forbidden not in lifecycle, f"lifecycle_forbidden:{forbidden}")

    require(
        lifecycle.count('"create",') == 2,
        "service_create_paths_not_fresh_and_restore",
    )
    commit_block = lifecycle[
        lifecycle.index("function Invoke-CommitPostInstall") :
        lifecycle.index("function Invoke-PreRemove")
    ]
    require(
        "Write-TransactionState $transaction" in commit_block,
        "post_install_finalization_not_journaled",
    )
    require(
        "Remove-TransactionState" not in commit_block,
        "checked_post_install_finalization_deletes_rollback_journal",
    )
    require(
        lifecycle.count(
            'Get-CimInstance -ClassName Win32_UserAccount -Filter '
            '"Name=\'scratchbird\' AND LocalAccount=TRUE"'
        )
        >= 2,
        "local_sam_user_not_rechecked_in_final_validator",
    )
    require(
        "ScratchBird must not create a local SAM service user" in smoke,
        "actual_smoke_local_sam_user_absence_check",
    )
    require(
        lifecycle.index("[Environment]::Is64BitProcess")
        < lifecycle.index('$LifecyclePhase = "PATH_VALIDATION"'),
        "windows_64bit_process_guard_order",
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
    ns = {
        "w": "http://wixtoolset.org/schemas/v4/wxs",
        "util": "http://wixtoolset.org/schemas/v4/wxs/util",
    }
    require(
        wix.count("[System64Folder]") == 7,
        "wix_x64_lifecycle_executable_reference_count",
    )
    require(
        wix.count("@SCRATCHBIRD_VERSION@") == 5,
        "wix_lifecycle_version_token_count",
    )
    require("[SystemFolder]" not in wix, "wix_32bit_powershell_forbidden")
    fragments = tree.findall("./w:Fragment", ns)
    require(len(fragments) == 1, "wix_lifecycle_fragment_not_atomic")
    fragment = fragments[0]
    require(
        fragment.find("./util:FailWhenDeferred", ns) is not None,
        "wix_fault_injection_not_wired",
    )
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
        for row in fragment.findall("./w:CustomAction", ns)
    }
    expected_actions = {
        "ScratchBirdRollbackPostInstall",
        "ScratchBirdPostInstall",
        "ScratchBirdFinalizePostInstall",
        "ScratchBirdCleanupPostInstall",
        "ScratchBirdRollbackPreRemove",
        "ScratchBirdPreRemove",
        "ScratchBirdCommitPreRemove",
    }
    require(set(actions) == expected_actions, "wix_lifecycle_action_set")
    for action_id in (
        "ScratchBirdRollbackPostInstall",
        "ScratchBirdRollbackPreRemove",
    ):
        require(
            actions[action_id].get("Execute") == "rollback"
            and actions[action_id].get("Return") == "ignore",
            f"wix_rollback_action_invalid:{action_id}",
        )
    for action_id in (
        "ScratchBirdPostInstall",
        "ScratchBirdFinalizePostInstall",
        "ScratchBirdPreRemove",
    ):
        require(
            actions[action_id].get("Execute") == "deferred"
            and actions[action_id].get("Return") == "check",
            f"wix_action_not_deferred:{action_id}",
        )
    for action_id in (
        "ScratchBirdCleanupPostInstall",
        "ScratchBirdCommitPreRemove",
    ):
        require(
            actions[action_id].get("Execute") == "commit"
            and actions[action_id].get("Return") == "ignore",
            f"wix_commit_action_invalid:{action_id}",
        )
    for action_id, action in actions.items():
        require(
            action.get("Impersonate") == "no",
            f"wix_action_impersonation:{action_id}",
        )
        require(
            action.get("HideTarget") == "yes",
            f"wix_action_target_visible:{action_id}",
        )
    set_properties = {
        row.get("Id"): row
        for row in fragment.findall("./w:SetProperty", ns)
    }
    require(
        set(set_properties) == expected_actions,
        "wix_lifecycle_property_set",
    )
    install_condition = 'NOT (REMOVE~="ALL")'
    remove_condition = 'REMOVE~="ALL" AND NOT UPGRADINGPRODUCTCODE'
    for action_id in (
        "ScratchBirdRollbackPostInstall",
        "ScratchBirdPostInstall",
        "ScratchBirdFinalizePostInstall",
    ):
        require(
            '-InstallRoot "[INSTALLFOLDER]."'
            in set_properties[action_id].get("Value", ""),
            f"wix_install_root_trailing_separator_guard:{action_id}",
        )
        require(
            set_properties[action_id].get("Condition") == install_condition,
            f"wix_install_property_condition:{action_id}",
        )
    install_cleanup = set_properties["ScratchBirdCleanupPostInstall"]
    require(
        install_cleanup.get("Condition") == install_condition,
        "wix_install_cleanup_condition",
    )
    require(
        install_cleanup.get("Value")
        == (
            '"[System64Folder]reg.exe" delete '
            '"HKLM\\SOFTWARE\\ScratchBird\\InstallerTransaction" /f'
        ),
        "wix_install_cleanup_not_fixed_exact_registry_delete",
    )
    require(
        "powershell" not in install_cleanup.get("Value", "").lower(),
        "wix_install_cleanup_payload_dependency",
    )
    for action_id in (
        "ScratchBirdRollbackPreRemove",
        "ScratchBirdPreRemove",
    ):
        require(
            '-InstallRoot "[INSTALLFOLDER]."'
            in set_properties[action_id].get("Value", ""),
            f"wix_install_root_trailing_separator_guard:{action_id}",
        )
        require(
            set_properties[action_id].get("Condition") == remove_condition,
            f"wix_remove_property_condition:{action_id}",
        )
    uninstall_commit = set_properties["ScratchBirdCommitPreRemove"]
    require(
        uninstall_commit.get("Condition") == remove_condition,
        "wix_remove_commit_condition",
    )
    require(
        uninstall_commit.get("Value")
        == (
            '"[System64Folder]reg.exe" delete '
            '"HKLM\\SOFTWARE\\ScratchBird\\InstallerTransaction" /f'
        ),
        "wix_remove_commit_not_fixed_exact_registry_delete",
    )
    require(
        "powershell" not in uninstall_commit.get("Value", "").lower(),
        "wix_remove_commit_payload_dependency",
    )
    for action_id, script_action in {
        "ScratchBirdRollbackPostInstall": "RollbackPostInstall",
        "ScratchBirdPostInstall": "PostInstall",
        "ScratchBirdFinalizePostInstall": "CommitPostInstall",
        "ScratchBirdRollbackPreRemove": "RollbackPreRemove",
        "ScratchBirdPreRemove": "PreRemove",
    }.items():
        require(
            f"-Action {script_action}"
            in set_properties[action_id].get("Value", ""),
            f"wix_script_action_mismatch:{action_id}",
        )
    scheduled = {
        row.get("Action"): row
        for row in fragment.findall(
            "./w:InstallExecuteSequence/w:Custom", ns
        )
    }
    require(
        set(scheduled) == expected_actions,
        "wix_lifecycle_actions_not_scheduled_in_linked_fragment",
    )
    for action_id in (
        "ScratchBirdRollbackPostInstall",
        "ScratchBirdPostInstall",
        "ScratchBirdFinalizePostInstall",
        "ScratchBirdCleanupPostInstall",
    ):
        require(
            scheduled[action_id].get("Condition") == install_condition,
            f"wix_install_sequence_condition:{action_id}",
        )
    for action_id in (
        "ScratchBirdRollbackPreRemove",
        "ScratchBirdPreRemove",
        "ScratchBirdCommitPreRemove",
    ):
        require(
            scheduled[action_id].get("Condition") == remove_condition,
            f"wix_remove_sequence_condition:{action_id}",
        )
    require(
        scheduled["ScratchBirdRollbackPostInstall"].get("Before")
        == "ScratchBirdPostInstall",
        "wix_post_install_rollback_order",
    )
    require(
        scheduled["ScratchBirdPostInstall"].get("Before")
        == "ScratchBirdFinalizePostInstall",
        "wix_post_install_fault_sequence",
    )
    require(
        scheduled["ScratchBirdFinalizePostInstall"].get("Before")
        == "ScratchBirdCleanupPostInstall",
        "wix_post_install_commit_sequence",
    )
    require(
        scheduled["ScratchBirdCleanupPostInstall"].get("Before")
        == "Wix4FailWhenDeferred_X64",
        "wix_post_install_cleanup_sequence",
    )
    require(
        scheduled["ScratchBirdRollbackPreRemove"].get("Before")
        == "ScratchBirdPreRemove",
        "wix_pre_remove_rollback_order",
    )
    require(
        scheduled["ScratchBirdPreRemove"].get("Before") == "RemoveFiles",
        "wix_pre_remove_sequence",
    )
    require(
        scheduled["ScratchBirdCommitPreRemove"].get("Before")
        == "Wix4FailWhenDeferred_X64",
        "wix_pre_remove_commit_sequence",
    )
    require(
        all(row.get("After") is None for row in scheduled.values()),
        "wix_lifecycle_action_has_unbounded_after_sequence",
    )
    require(
        "SB_INSTALLER_USER" not in wix and "InstallerUser" not in wix,
        "wix_installer_user_surface_forbidden",
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
            "function New-ShortAdministrativeExtractRoot",
            "if ($path.Length -gt 48)",
            "$script:AdministrativeExtractRoot = $payloadRoot",
            'Get-ChildItem -Path $payloadRoot -Recurse -Filter "NATIVE_RELEASE_PROFILE.json"',
            "administrative-extract-cleanup-proof.json",
            "without a pre-existing ScratchBird SAM identity",
            "Assert-InstalledWindowsSystem -RequireGroupCreatedByThisRun",
            "filesystem_operations_group_preserved_after_uninstall",
            '$group.PSBase.Invoke("Members")',
            "ScratchBird filesystem-operations group must have no members",
            "$evidence.filesystem_operations_group_member_count -ne 0",
            "$evidence.filesystem_operations_group_creation_policy",
            "$evidence.filesystem_operations_group_created_by_this_run",
            "$evidence.lifecycle_process_architecture",
            "param([switch] $RequireGroupCreatedByThisRun)",
            "Fresh MSI install did not create the ScratchBird group",
            "service_local_sam_group_membership = $false",
            "human_service_group_membership_mutated = $false",
            'create_time_os_authorization = "administrator_only"',
            "filesystem_directory_and_process_execution_only_no_database_or_security_authority",
            "NT SERVICE\\scratchbird",
            "--config-mode system-defaults",
            "--config-mode system-installed",
            "@SCRATCHBIRD_|operator_required|compatibility|emulation|firebird|mysql|postgres",
            "service_fresh_install = \"manual_stopped\"",
            "scm_runtime_start_proof = \"not_claimed_by_installer_lifecycle_smoke\"",
            '$InstallerTransactionRegistryPath = '
            '"HKLM:\\SOFTWARE\\ScratchBird\\InstallerTransaction"',
            "function Invoke-MsiExpectedFailure",
            "if ($process.ExitCode -ne 1603)",
            "function Assert-MsiLogContainsTokens",
            "function Assert-InstallerTransactionJournalAbsent",
            "function Assert-NoScratchBirdIdentityAndJournal",
            "function Get-ScratchBirdIdentitySnapshot",
            "function Assert-ScratchBirdIdentitySnapshot",
            "function Get-ScratchBirdServiceSecuritySddl",
            "function ConvertTo-NormalizedScratchBirdSddl",
            "WIXFAILWHENDEFERRED=1",
            "msi-fault-injected-fresh-install.log",
            "msi-fault-injected-uninstall.log",
            "Wix4FailWhenDeferred_X64",
            "ScratchBirdFinalizePostInstall",
            "ScratchBirdCleanupPostInstall",
            "qa-failed-install-retry-preserve.conf",
            "service_security_sddl",
            "registry_sddl",
            "delayed_auto_start_present",
            "fault_injected_fresh_install = "
            '"failed_as_expected_identity_rollback_passed"',
            "fault_injected_uninstall = "
            '"failed_as_expected_snapshotted_service_fields_restore_and_'
            'preserved_group_verification_passed"',
            "installer_transaction_rollback_scope = "
            '"service_and_filesystem_operations_group_identity_only"',
            "failed_install_programdata_configuration_and_acl_policy = "
            '"preserved_not_rolled_back_and_required_acl_reapplied_on_retry"',
            "service_snapshotted_identity_configuration_and_runtime_state_"
            "fields_restored_after_failed_uninstall = $true",
            "filesystem_operations_group_preserved_during_failed_uninstall = "
            "$true",
            "post_install_identity_finalization = "
            '"checked_deferred_before_install_finalize"',
            "post_install_journal_cleanup = "
            '"ignored_commit_after_successful_install_finalize_'
            'fixed_absolute_System32_reg.exe_exact_key_delete"',
            "pre_remove_journal_cleanup = "
            '"ignored_commit_after_successful_install_finalize_'
            'fixed_absolute_System32_reg.exe_exact_key_delete"',
        ),
        "smoke",
    )
    require(
        "$env:USERNAME" not in smoke and "$env:USERDOMAIN" not in smoke,
        "smoke_ambient_installer_user",
    )
    require(
        'Join-Path $WorkRoot "administrative-extract"' not in smoke,
        "msi_administrative_extract_workspace_path_forbidden",
    )
    require(
        'Get-ChildItem -Path $WorkRoot -Recurse -Filter "NATIVE_RELEASE_PROFILE.json"'
        not in smoke,
        "msi_native_profile_workspace_search_forbidden",
    )
    require(
        smoke.index('"/a"') < smoke.index('"/i"'),
        "administrative_extract_not_separate",
    )
    require(
        smoke.count('"WIXFAILWHENDEFERRED=1"') == 2,
        "fault_injection_install_uninstall_count",
    )
    require(
        smoke.index("msi-fault-injected-fresh-install.log")
        < smoke.index('Join-Path $WorkRoot "msi-actual-install.log"'),
        "fresh_install_fault_not_before_normal_install",
    )
    require(
        smoke.index("msi-fault-injected-uninstall.log")
        < smoke.index('Join-Path $WorkRoot "msi-actual-uninstall.log"'),
        "uninstall_fault_not_before_normal_uninstall",
    )

    require_tokens(
        builder,
        (
            "def stage_windows_system_install_tree(",
            "def write_windows_system_install_profile(",
            "def write_windows_system_package_evidence(",
            "def materialize_windows_wix_lifecycle(",
            "WINDOWS_NATIVE_CONFIGS = (",
            '"SBbootstrap.profile"',
            '"windows-system-payload"',
            "stage_windows_system_install_tree(",
            "write_windows_system_package_evidence(",
            '"WixToolset.Util.wixext"',
            '<CustomActionRef Id="ScratchBirdPostInstall" />',
            '<Launch Condition="NOT RollbackDisabled" '
            'Message="ScratchBird requires Windows Installer rollback to be enabled." />',
            'if text.count("@SCRATCHBIRD_VERSION@") != 5:',
            '"native_default_port": 3092',
            r'service_identity = NT SERVICE\scratchbird',
            "@SCRATCHBIRD_STATE_ROOT@",
            "@SCRATCHBIRD_INSTALL_ROOT@",
        ),
        "builder",
    )
    require_tokens(
        workflow,
        (
            "Set up WiX and utility extension",
            "Verify Windows native local-group lifecycle dependency",
            'System32\\WindowsPowerShell\\v1.0\\powershell.exe',
            "System.Management.Automation.Language.Parser]::ParseFile",
            "$env:SB_SMOKE_PARSE_PATH",
            'windows_native_local_group_dependency=passed',
            "$systemDirectory = [Environment]::SystemDirectory",
            '$candidate = Join-Path $canonicalSystemDirectory "net.exe"',
            "wix extension add -g",
            "dotnet tool install --global wix --version 4.0.6",
            "WixToolset.Util.wixext/4.0.6",
            "--require-msi",
            'throw "No Windows MSI package found"',
        ),
        "workflow",
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
