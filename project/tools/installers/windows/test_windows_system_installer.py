#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Fixture tests for Windows native system payload and WiX assembly."""

from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET


REPO_ROOT = Path(__file__).resolve().parents[4]
BUILDER_PATH = REPO_ROOT / "project" / "tools" / "installers" / "build_installers.py"
SPEC = importlib.util.spec_from_file_location("scratchbird_build_installers", BUILDER_PATH)
assert SPEC is not None and SPEC.loader is not None
installers = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(installers)
QA_HELPER_PATH = (
    REPO_ROOT
    / "project"
    / "examples"
    / "native_release_qa"
    / "prepare_native_qa_instance.py"
)
QA_SPEC = importlib.util.spec_from_file_location(
    "scratchbird_prepare_native_qa", QA_HELPER_PATH
)
assert QA_SPEC is not None and QA_SPEC.loader is not None
qa_helper = importlib.util.module_from_spec(QA_SPEC)
QA_SPEC.loader.exec_module(qa_helper)


class WindowsSystemInstallerTest(unittest.TestCase):
    def make_portable(self, root: Path) -> Path:
        portable = root / "portable"
        runtime = portable / "opt" / "ScratchBird"
        for directory in ("bin", "lib", "share/scratchbird/resources"):
            (runtime / directory).mkdir(parents=True, exist_ok=True)
        for binary in ("SBsrv", "SBgate", "SBParser", "SBmgr"):
            (runtime / "bin" / f"{binary}.exe").write_bytes(binary.encode())
        (runtime / "lib" / "sb_core.dll").write_bytes(b"fixture")
        (runtime / "share" / "scratchbird" / "resources" / "fixture.json").write_text(
            "{}\n", encoding="utf-8"
        )
        config = portable / "etc" / "scratchbird"
        config.mkdir(parents=True)
        templates = REPO_ROOT / "project" / "config" / "templates"
        for name in installers.WINDOWS_NATIVE_CONFIGS:
            (config / name).write_text(
                (templates / name).read_text(encoding="utf-8"),
                encoding="utf-8",
            )
        return portable

    def make_staged_artifact(self, root: Path) -> Path:
        portable = self.make_portable(root)
        artifact = root / "artifact"
        runtime = portable / "opt" / "ScratchBird"
        for name in ("bin", "lib", "share"):
            shutil.copytree(runtime / name, artifact / name)
        shutil.copytree(portable / "etc", artifact / "etc")
        (artifact / "STANDALONE_OUTPUT_MANIFEST.json").write_text(
            json.dumps(
                {
                    "platform": "windows",
                    "distribution_profile": "native-sbsql-only",
                    "emulation_components": "excluded",
                }
            )
            + "\n",
            encoding="utf-8",
        )
        (artifact / "NATIVE_RELEASE_PROFILE.json").write_text(
            json.dumps(
                {
                    "schema_id": "scratchbird.native_release_profile.v1",
                    "profile": "native-sbsql-only",
                    "platform": "windows",
                }
            )
            + "\n",
            encoding="utf-8",
        )
        return artifact

    def test_system_tree_is_canonical_and_portable_tree_is_unchanged(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            portable = self.make_portable(root)
            system = root / "system"
            installers.stage_windows_system_install_tree(
                portable, system, "1.2.3-nightly", "fixture-build"
            )

            self.assertTrue((portable / "opt" / "ScratchBird" / "bin").is_dir())
            self.assertTrue((portable / "etc" / "scratchbird").is_dir())
            self.assertTrue((system / "bin" / "SBsrv.exe").is_file())
            self.assertTrue((system / "bin" / "SBmgr.exe").is_file())
            self.assertFalse((system / "opt").exists())
            self.assertFalse((system / "etc").exists())
            self.assertTrue(
                (
                    system
                    / "libexec"
                    / "scratchbird-windows-system-install.ps1"
                ).is_file()
            )

            defaults = system / installers.WINDOWS_CONFIG_DEFAULTS_REL
            self.assertEqual(
                {path.name for path in defaults.iterdir()},
                set(installers.WINDOWS_NATIVE_CONFIGS),
            )
            all_text = "\n".join(
                (defaults / name).read_text(encoding="utf-8")
                for name in installers.WINDOWS_NATIVE_CONFIGS
            )
            self.assertNotIn("3050", all_text)
            self.assertIn("port = 3092", all_text)
            self.assertIn("@SCRATCHBIRD_STATE_ROOT@/data/default.sbdb", all_text)
            self.assertIn(
                "@SCRATCHBIRD_INSTALL_ROOT@/bin/SBgate.exe", all_text
            )
            self.assertIn(
                "@SCRATCHBIRD_INSTALL_ROOT@/bin/SBParser.exe", all_text
            )
            self.assertIn("platform = windows", all_text)
            self.assertIn(r"service_identity = NT SERVICE\scratchbird", all_text)
            self.assertIn("service_group = ScratchBird", all_text)

            release = system / "share" / "scratchbird" / "release"
            profile = json.loads(
                (release / installers.WINDOWS_SYSTEM_PROFILE_ASSET).read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(profile["version"], "1.2.3-nightly")
            self.assertEqual(profile["build_id"], "fixture-build")
            self.assertEqual(profile["native_default_port"], 3092)
            self.assertEqual(
                profile["os_identity"]["service_authority_scope"],
                "filesystem_directory_and_process_execution_only_"
                "no_database_or_security_authority",
            )
            self.assertEqual(
                profile["os_identity"]["service_local_sam_group_membership"],
                "forbidden_and_verified_absent_across_all_local_groups",
            )
            self.assertEqual(
                profile["os_identity"]["group_creation_mechanism"],
                "absolute_System32_net.exe_localgroup_add",
            )
            self.assertEqual(
                profile["os_identity"]["group_creation_process_architecture"],
                "64_bit_required",
            )
            self.assertEqual(
                profile["create_time_os_authorization"], "administrator_only"
            )
            self.assertEqual(
                profile["human_service_group_membership_mutation"], "forbidden"
            )
            self.assertEqual(
                profile["os_identity"]["group_purpose"],
                "filesystem_operations_only_no_database_or_security_authority",
            )
            self.assertEqual(
                profile["os_identity"]["group_membership_policy"],
                "must_be_empty_no_human_or_service_members",
            )
            self.assertTrue(profile["service"]["create_if_missing"])
            self.assertEqual(
                profile["service"]["creation_mechanism"],
                "elevated_deferred_msi_lifecycle_helper_Ensure-SBsrvService",
            )
            transaction = profile["installer_transaction"]
            self.assertTrue(transaction["rollback_required"])
            self.assertEqual(
                transaction["rollback_disabled_policy"],
                "blocked_by_package_launch_condition",
            )
            self.assertEqual(
                transaction["journal"],
                r"HKLM\SOFTWARE\ScratchBird\InstallerTransaction",
            )
            self.assertEqual(
                transaction["rollback_scope"],
                "service_and_filesystem_operations_group_identity_only",
            )
            self.assertEqual(
                transaction["fresh_install_failure"],
                "remove_service_and_group_created_by_install_attempt",
            )
            self.assertEqual(
                transaction["uninstall_failure"],
                "restore_snapshotted_service_identity_configuration_and_"
                "runtime_state_fields_and_verify_preserved_group_identity",
            )
            self.assertEqual(
                transaction["programdata_configuration_and_acl_policy"],
                "preserved_not_rolled_back_and_required_acl_reapplied_on_retry",
            )
            self.assertEqual(
                transaction["post_install_identity_finalization"],
                "checked_deferred_before_install_finalize",
            )
            self.assertEqual(
                transaction["post_install_journal_cleanup"],
                "ignored_commit_after_successful_install_finalize_"
                "fixed_absolute_System32_reg.exe_exact_key_delete",
            )
            self.assertEqual(
                transaction["pre_remove_journal_cleanup"],
                "ignored_commit_after_successful_install_finalize_"
                "fixed_absolute_System32_reg.exe_exact_key_delete",
            )
            self.assertEqual(
                transaction["fault_injection"],
                "WIXFAILWHENDEFERRED=1",
            )
            topology = profile["topology"]
            self.assertEqual(
                topology["native_route"],
                "client_to_optional_SBmgr_not_used_with_emulation_to_shared_"
                "SBgate_to_standalone_selected_SBParser_to_SBPS_IPC_to_"
                "SBsrv_engine",
            )
            self.assertEqual(
                topology["listener_model"],
                "one_shared_listener_executable_for_all_parser_families",
            )
            self.assertEqual(
                topology["parser_process_model"],
                "one_standalone_parser_for_the_selected_dialect",
            )
            self.assertEqual(topology["parser_engine_transport"], "sbps_ipc_only")
            self.assertEqual(topology["direct_engine_link"], "forbidden")
            self.assertEqual(topology["cross_parser_dependency"], "forbidden")
            self.assertEqual(
                topology["manager"],
                "optional_SBmgr_disabled_by_default_bypassed_not_used_with_"
                "emulation_not_installed_as_a_service",
            )
            metadata = json.loads(
                (release / "INSTALL_MANIFEST.json").read_text(encoding="utf-8")
            )
            self.assertEqual(
                metadata["install_roots"]["runtime"],
                r"%ProgramFiles%\ScratchBird",
            )
            self.assertEqual(
                metadata["install_roots"]["configuration"],
                r"%ProgramData%\ScratchBird\config",
            )

    def test_wix_recipe_wires_privileged_lifecycle_fragment(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            portable = self.make_portable(root)
            system = root / "system"
            output = root / "output"
            output.mkdir()
            installers.stage_windows_system_install_tree(
                portable, system, "1.2.3-nightly", "fixture-build"
            )
            with mock.patch.object(installers.shutil, "which", return_value=None):
                recipes = installers.make_wix_msi(
                    system, output, "1.2.3-nightly", require_msi=False
                )
            self.assertEqual(
                {path.name for path in recipes},
                {"scratchbird.wxs", "scratchbird-windows-lifecycle.wxs"},
            )
            main = (output / "scratchbird.wxs").read_text(encoding="utf-8")
            lifecycle = (
                output / "scratchbird-windows-lifecycle.wxs"
            ).read_text(encoding="utf-8")
            self.assertNotIn("ScratchBirdWindowsLifecycleComponents", main)
            self.assertIn(str(system / "bin" / "SBsrv.exe"), main)
            self.assertNotIn(str(portable / "opt"), main)
            self.assertNotIn("SB_INSTALLER_USER", lifecycle)
            self.assertNotIn("InstallerUser", lifecycle)
            self.assertIn('Execute="deferred"', lifecycle)
            self.assertIn('Execute="rollback"', lifecycle)
            self.assertIn('Execute="commit"', lifecycle)
            self.assertIn('Impersonate="no"', lifecycle)
            self.assertEqual(lifecycle.count("[System64Folder]"), 7)
            self.assertNotIn("[SystemFolder]", lifecycle)
            self.assertIn(
                '-InstallRoot &quot;[INSTALLFOLDER].&quot;', lifecycle
            )
            self.assertNotIn(
                '-InstallRoot &quot;[INSTALLFOLDER]&quot;', lifecycle
            )
            self.assertIn("NOT UPGRADINGPRODUCTCODE", lifecycle)
            self.assertNotIn("@SCRATCHBIRD_VERSION@", lifecycle)
            ns = {
                "w": "http://wixtoolset.org/schemas/v4/wxs",
                "util": "http://wixtoolset.org/schemas/v4/wxs/util",
            }
            main_tree = ET.fromstring(main)
            lifecycle_tree = ET.fromstring(lifecycle)
            launch = main_tree.find("./w:Package/w:Launch", ns)
            self.assertIsNotNone(launch)
            self.assertEqual(launch.get("Condition"), "NOT RollbackDisabled")
            self.assertIn("rollback", launch.get("Message", "").lower())
            refs = {
                row.get("Id")
                for row in main_tree.findall(
                    "./w:Package/w:CustomActionRef", ns
                )
            }
            self.assertEqual(refs, {"ScratchBirdPostInstall"})

            fragments = lifecycle_tree.findall("./w:Fragment", ns)
            self.assertEqual(len(fragments), 1)
            fragment = fragments[0]
            self.assertIsNotNone(fragment.find("./util:FailWhenDeferred", ns))
            action_ids = {
                row.get("Id")
                for row in fragment.findall("./w:CustomAction", ns)
            }
            property_ids = {
                row.get("Id")
                for row in fragment.findall("./w:SetProperty", ns)
            }
            self.assertEqual(
                action_ids,
                {
                    "ScratchBirdRollbackPostInstall",
                    "ScratchBirdPostInstall",
                    "ScratchBirdFinalizePostInstall",
                    "ScratchBirdCleanupPostInstall",
                    "ScratchBirdRollbackPreRemove",
                    "ScratchBirdPreRemove",
                    "ScratchBirdCommitPreRemove",
                },
            )
            self.assertEqual(property_ids, action_ids)
            self.assertTrue(refs.issubset(action_ids))
            actions = {
                row.get("Id"): row
                for row in fragment.findall("./w:CustomAction", ns)
            }
            for action_id in (
                "ScratchBirdRollbackPostInstall",
                "ScratchBirdRollbackPreRemove",
            ):
                self.assertEqual(actions[action_id].get("Execute"), "rollback")
                self.assertEqual(actions[action_id].get("Return"), "ignore")
            for action_id in (
                "ScratchBirdPostInstall",
                "ScratchBirdFinalizePostInstall",
                "ScratchBirdPreRemove",
            ):
                self.assertEqual(actions[action_id].get("Execute"), "deferred")
                self.assertEqual(actions[action_id].get("Return"), "check")
            for action_id in (
                "ScratchBirdCleanupPostInstall",
                "ScratchBirdCommitPreRemove",
            ):
                self.assertEqual(actions[action_id].get("Execute"), "commit")
                self.assertEqual(actions[action_id].get("Return"), "ignore")
            for action in actions.values():
                self.assertEqual(action.get("Impersonate"), "no")
                self.assertEqual(action.get("HideTarget"), "yes")

            set_properties = {
                row.get("Id"): row
                for row in fragment.findall("./w:SetProperty", ns)
            }
            install_condition = 'NOT (REMOVE~="ALL")'
            remove_condition = (
                'REMOVE~="ALL" AND NOT UPGRADINGPRODUCTCODE'
            )
            for action_id in (
                "ScratchBirdRollbackPostInstall",
                "ScratchBirdPostInstall",
                "ScratchBirdFinalizePostInstall",
            ):
                row = set_properties[action_id]
                self.assertEqual(row.get("Condition"), install_condition)
                self.assertIn(
                    '-InstallRoot "[INSTALLFOLDER]."', row.get("Value", "")
                )
            install_cleanup = set_properties[
                "ScratchBirdCleanupPostInstall"
            ]
            self.assertEqual(
                install_cleanup.get("Condition"), install_condition
            )
            self.assertEqual(
                install_cleanup.get("Value"),
                '"[System64Folder]reg.exe" delete '
                '"HKLM\\SOFTWARE\\ScratchBird\\InstallerTransaction" /f',
            )
            self.assertNotIn(
                "powershell", install_cleanup.get("Value", "").lower()
            )
            for action_id in (
                "ScratchBirdRollbackPreRemove",
                "ScratchBirdPreRemove",
            ):
                row = set_properties[action_id]
                self.assertEqual(row.get("Condition"), remove_condition)
                self.assertIn(
                    '-InstallRoot "[INSTALLFOLDER]."', row.get("Value", "")
                )
            uninstall_commit = set_properties["ScratchBirdCommitPreRemove"]
            self.assertEqual(
                uninstall_commit.get("Condition"), remove_condition
            )
            self.assertEqual(
                uninstall_commit.get("Value"),
                '"[System64Folder]reg.exe" delete '
                '"HKLM\\SOFTWARE\\ScratchBird\\InstallerTransaction" /f',
            )
            self.assertNotIn(
                "powershell", uninstall_commit.get("Value", "").lower()
            )
            expected_script_actions = {
                "ScratchBirdRollbackPostInstall": "RollbackPostInstall",
                "ScratchBirdPostInstall": "PostInstall",
                "ScratchBirdFinalizePostInstall": "CommitPostInstall",
                "ScratchBirdRollbackPreRemove": "RollbackPreRemove",
                "ScratchBirdPreRemove": "PreRemove",
            }
            for action_id, script_action in expected_script_actions.items():
                self.assertIn(
                    f"-Action {script_action}",
                    set_properties[action_id].get("Value", ""),
                )

            scheduled = {
                row.get("Action"): row
                for row in fragment.findall(
                    "./w:InstallExecuteSequence/w:Custom", ns
                )
            }
            self.assertEqual(set(scheduled), action_ids)
            for action_id in (
                "ScratchBirdRollbackPostInstall",
                "ScratchBirdPostInstall",
                "ScratchBirdFinalizePostInstall",
                "ScratchBirdCleanupPostInstall",
            ):
                self.assertEqual(
                    scheduled[action_id].get("Condition"), install_condition
                )
            for action_id in (
                "ScratchBirdRollbackPreRemove",
                "ScratchBirdPreRemove",
                "ScratchBirdCommitPreRemove",
            ):
                self.assertEqual(
                    scheduled[action_id].get("Condition"), remove_condition
                )
            self.assertEqual(
                scheduled["ScratchBirdRollbackPostInstall"].get("Before"),
                "ScratchBirdPostInstall",
            )
            self.assertEqual(
                scheduled["ScratchBirdPostInstall"].get("Before"),
                "ScratchBirdFinalizePostInstall",
            )
            self.assertEqual(
                scheduled["ScratchBirdFinalizePostInstall"].get("Before"),
                "ScratchBirdCleanupPostInstall",
            )
            self.assertEqual(
                scheduled["ScratchBirdCleanupPostInstall"].get("Before"),
                "Wix4FailWhenDeferred_X64",
            )
            self.assertEqual(
                scheduled["ScratchBirdRollbackPreRemove"].get("Before"),
                "ScratchBirdPreRemove",
            )
            self.assertEqual(
                scheduled["ScratchBirdPreRemove"].get("Before"), "RemoveFiles"
            )
            self.assertEqual(
                scheduled["ScratchBirdCommitPreRemove"].get("Before"),
                "Wix4FailWhenDeferred_X64",
            )
            self.assertTrue(
                all(row.get("After") is None for row in scheduled.values())
            )

    @unittest.skipUnless(
        os.name == "nt" and shutil.which("wix"),
        "WiX MSI compilation requires Windows",
    )
    def test_wix_tool_compiles_materialized_msi(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            portable = self.make_portable(root)
            system = root / "system"
            output = root / "output"
            output.mkdir()
            installers.stage_windows_system_install_tree(
                portable, system, "1.2.3-nightly", "fixture-build"
            )
            artifacts = installers.make_wix_msi(
                system, output, "1.2.3-nightly", require_msi=True
            )
            self.assertIn(
                output / "scratchbird-windows-1.2.3-nightly.msi",
                artifacts,
            )
            self.assertTrue(
                (output / "scratchbird-windows-1.2.3-nightly.msi").is_file()
            )

    def test_windows_evidence_requires_actual_msi_smoke(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            path = installers.write_windows_system_package_evidence(
                output, "1.2.3-nightly", "fixture-build"
            )
            evidence = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(evidence["native_default_port"], 3092)
            self.assertEqual(evidence["service"]["name"], "scratchbird")
            self.assertEqual(
                evidence["service"]["account"], r"NT SERVICE\scratchbird"
            )
            self.assertFalse(
                evidence["service"]["parser_or_listener_services_installed"]
            )
            self.assertFalse(evidence["service"]["manager_service_installed"])
            self.assertFalse(
                evidence["os_identity"]["local_sam_group_membership"]
            )
            self.assertEqual(
                evidence["os_identity"]["service_authority_scope"],
                "filesystem_directory_and_process_execution_only_"
                "no_database_or_security_authority",
            )
            self.assertEqual(
                evidence["os_identity"]["create_time_os_authorization"],
                "administrator_only",
            )
            self.assertFalse(
                evidence["os_identity"]["human_service_group_membership_mutation"]
            )
            self.assertTrue(
                evidence["service"]["fresh_install_creates_missing_service"]
            )
            self.assertEqual(
                evidence["service"]["creation_mechanism"],
                "elevated_deferred_msi_lifecycle_helper_Ensure-SBsrvService",
            )
            self.assertFalse(evidence["database_files_created"])
            self.assertFalse(evidence["security_sidecars_created"])
            transaction = evidence["installer_transaction"]
            self.assertTrue(transaction["rollback_required"])
            self.assertEqual(
                transaction["rollback_disabled_policy"],
                "blocked_by_package_launch_condition",
            )
            self.assertEqual(
                transaction["journal"],
                r"HKLM\SOFTWARE\ScratchBird\InstallerTransaction",
            )
            self.assertEqual(
                transaction["rollback_scope"],
                "service_and_filesystem_operations_group_identity_only",
            )
            self.assertEqual(
                transaction["uninstall_failure"],
                "restore_snapshotted_service_identity_configuration_and_"
                "runtime_state_fields_and_verify_preserved_group_identity",
            )
            self.assertEqual(
                transaction["programdata_configuration_and_acl_policy"],
                "preserved_not_rolled_back_and_required_acl_reapplied_on_retry",
            )
            self.assertEqual(
                transaction["post_install_identity_finalization"],
                "checked_deferred_before_install_finalize",
            )
            self.assertEqual(
                transaction["post_install_journal_cleanup"],
                "ignored_commit_after_successful_install_finalize_"
                "fixed_absolute_System32_reg.exe_exact_key_delete",
            )
            self.assertEqual(
                transaction["pre_remove_journal_cleanup"],
                "ignored_commit_after_successful_install_finalize_"
                "fixed_absolute_System32_reg.exe_exact_key_delete",
            )
            self.assertEqual(
                evidence["verification"],
                "pending_windows_actual_msi_install_smoke",
            )

    def test_builder_cli_and_artifact_verifier_fixture(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            artifact = self.make_staged_artifact(root)
            output = root / "output"
            command = [
                sys.executable,
                str(BUILDER_PATH),
                "--platform",
                "windows",
                "--artifact-root",
                str(artifact),
                "--output-root",
                str(output),
                "--version",
                "1.2.3-nightly",
                "--build-id",
                "fixture-build",
                "--require-native-only",
            ]
            result = subprocess.run(
                command,
                cwd=REPO_ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertTrue(
                (output / "scratchbird-windows-1.2.3-nightly.zip").is_file()
            )
            self.assertTrue((output / "scratchbird.wxs").is_file())
            self.assertTrue(
                (output / "scratchbird-windows-lifecycle.wxs").is_file()
            )
            self.assertTrue(
                (output / "WINDOWS_SYSTEM_PACKAGE_EVIDENCE.json").is_file()
            )
            verify = subprocess.run(
                [
                    sys.executable,
                    str(
                        REPO_ROOT
                        / "project"
                        / "tools"
                        / "installers"
                        / "verify_installer_artifacts.py"
                    ),
                    "--platform",
                    "windows",
                    "--artifact-root",
                    str(output),
                ],
                cwd=REPO_ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(verify.returncode, 0, verify.stdout)

    def test_system_tree_rejects_non_native_config_entries(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            portable = self.make_portable(root)
            (portable / "etc" / "scratchbird" / "Firebird.conf").write_text(
                "port = 3050\n", encoding="utf-8"
            )
            with self.assertRaises(SystemExit):
                installers.stage_windows_system_install_tree(
                    portable,
                    root / "system",
                    "1.2.3-nightly",
                    "fixture-build",
                )

    def test_system_qa_activation_refuses_operator_config_changes(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            portable = self.make_portable(root)
            system = root / "system"
            state = root / "state"
            installers.stage_windows_system_install_tree(
                portable, system, "1.2.3-nightly", "fixture-build"
            )
            defaults = system / installers.WINDOWS_CONFIG_DEFAULTS_REL
            live = state / "config"
            live.mkdir(parents=True)
            for name in installers.WINDOWS_NATIVE_CONFIGS:
                expected = qa_helper.materialize_windows_system_default(
                    (defaults / name).read_text(encoding="utf-8"),
                    system,
                    state,
                )
                (live / name).write_text(expected, encoding="utf-8")
            before = {
                path.name: path.read_bytes() for path in sorted(live.iterdir())
            }
            selected = qa_helper.prepare_windows_system_config_root(
                system, state, defaults
            )
            self.assertEqual(selected, live.resolve())
            self.assertEqual(
                before,
                {
                    path.name: path.read_bytes()
                    for path in sorted(live.iterdir())
                },
            )
            (live / "SBsrv.conf").write_text(
                "# operator-owned change\n", encoding="utf-8"
            )
            with self.assertRaises(SystemExit):
                qa_helper.prepare_windows_system_config_root(
                    system, state, defaults
                )

    def test_readme_binds_start_service_to_canonical_programdata_mode(self) -> None:
        readme = (
            REPO_ROOT
            / "project"
            / "examples"
            / "native_release_qa"
            / "README.md"
        ).read_text(encoding="utf-8")
        self.assertIn("--windows-system-service", readme)
        self.assertIn(
            r"$env:ProgramData\ScratchBird\config\SBsrv.conf", readme
        )
        self.assertIn(
            "For a private portable instance, do not run `Start-Service`",
            readme,
        )
        helper = QA_HELPER_PATH.read_text(encoding="utf-8")
        self.assertIn("windows_system_service_canonical_roots_required", helper)
        self.assertIn("windows_system_existing_config_mismatch", helper)
        self.assertIn("windows_system_service_must_be_stopped", helper)
        self.assertIn("protected_service_registry_environment", helper)
        lifecycle = (
            REPO_ROOT
            / "project/tools/installers/windows/"
            "scratchbird-windows-system-install.ps1"
        ).read_text(encoding="utf-8")
        self.assertIn('Win32_Group -Filter "LocalAccount=TRUE"', lifecycle)
        self.assertGreaterEqual(
            lifecycle.count(
                'Get-CimInstance -ClassName Win32_UserAccount -Filter '
                '"Name=\'scratchbird\' AND LocalAccount=TRUE"'
            ),
            2,
        )
        self.assertIn('$group.PSBase.Invoke("Members")', lifecycle)
        self.assertIn("if ($members.Count -ne 0)", lifecycle)
        self.assertIn("filesystem_operations_group_member_count = 0", lifecycle)
        self.assertIn(
            'filesystem_operations_group_creation_policy = "absolute_System32_net.exe_localgroup_add_when_missing"',
            lifecycle,
        )
        self.assertIn(
            "filesystem_operations_group_created_by_this_run = [bool]$GroupCreatedByThisRun",
            lifecycle,
        )
        self.assertIn('lifecycle_process_architecture = "64_bit"', lifecycle)
        self.assertIn(
            "service_local_sam_group_membership = $false", lifecycle
        )
        self.assertIn('$LifecyclePhase = "PRECHECK"', lifecycle)
        self.assertIn("[Environment]::Is64BitProcess", lifecycle)
        self.assertIn(
            '$GroupName = "ScratchBird"',
            lifecycle,
        )
        self.assertIn(
            '$GroupDescription = "ScratchBird filesystem operations group; no database or security authority"',
            lifecycle,
        )
        self.assertIn(
            "function Get-SystemNetExecutable",
            lifecycle,
        )
        self.assertIn(
            "$systemDirectory = [Environment]::SystemDirectory",
            lifecycle,
        )
        self.assertIn(
            '$candidate = Join-Path $canonicalSystemDirectory "net.exe"',
            lifecycle,
        )
        self.assertIn(
            "[IO.FileAttributes]::ReparsePoint",
            lifecycle,
        )
        self.assertIn(
            "[IO.Path]::GetDirectoryName($canonicalCandidate)",
            lifecycle,
        )
        self.assertIn(
            "[IO.Path]::GetFileName($canonicalCandidate)",
            lifecycle,
        )
        self.assertIn(
            "function Get-PostInstallGroupComment",
            lifecycle,
        )
        self.assertIn(
            '& $net "localgroup" $GroupName "/add" '
            '"/comment:$transactionComment" 1>$null 2>$null',
            lifecycle,
        )
        self.assertIn(
            "$nativeStatus = [int]$LASTEXITCODE",
            lifecycle,
        )
        self.assertIn(
            "if ($nativeStatus -ne 0)",
            lifecycle,
        )
        self.assertIn(
            "[Globalization.CultureInfo]::InvariantCulture",
            lifecycle,
        )
        self.assertIn(
            '$script:LifecyclePhase = "GROUP_IDENTITY_CREATE_EXIT_$nativeStatusText"',
            lifecycle,
        )
        for phase in (
            "GROUP_IDENTITY_NATIVE_PATH",
            "GROUP_IDENTITY_CREATE",
            "GROUP_IDENTITY_POSTFAILURE_INVENTORY",
            "GROUP_IDENTITY_FINAL_VALIDATE",
        ):
            self.assertIn(f'$script:LifecyclePhase = "{phase}"', lifecycle)
        self.assertIn(
            '"BOOTSTRAP.GROUP_CREATE_FAILED.$creationFailurePhase"', lifecycle
        )
        self.assertNotIn('$computer.Create("group", $GroupName)', lifecycle)
        self.assertNotIn("$group.SetInfo()", lifecycle)
        self.assertNotIn("Microsoft.PowerShell.LocalAccounts", lifecycle)
        self.assertNotIn("New-LocalGroup", lifecycle)
        self.assertLess(
            lifecycle.index("[Environment]::Is64BitProcess"),
            lifecycle.index("$LifecyclePhase = \"PATH_VALIDATION\""),
        )
        self.assertIn(
            '"BOOTSTRAP.INSTALL_DEFAULTS_INVALID.$LifecyclePhase"',
            lifecycle,
        )
        self.assertNotIn("$_.Exception", lifecycle)
        for token in (
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
        ):
            self.assertIn(token, lifecycle)
        commit_block = lifecycle[
            lifecycle.index("function Invoke-CommitPostInstall") :
            lifecycle.index("function Invoke-PreRemove")
        ]
        self.assertIn("Write-TransactionState $transaction", commit_block)
        self.assertNotIn("Remove-TransactionState", commit_block)
        self.assertIn("SetAccessRuleProtection($true, $false)", lifecycle)
        self.assertIn('"S-1-5-18"', lifecycle)
        self.assertIn('"S-1-5-32-544"', lifecycle)
        self.assertNotIn("$ServiceCreatedByThisRun", lifecycle)
        self.assertNotIn("function Rollback-CreatedService", lifecycle)
        smoke = (
            REPO_ROOT / "project/tools/installers/smoke_install_windows.ps1"
        ).read_text(encoding="utf-8")
        self.assertIn(
            "ScratchBird must not create a local SAM service user", smoke
        )
        self.assertIn("function New-ShortAdministrativeExtractRoot", smoke)
        self.assertIn("if ($path.Length -gt 48)", smoke)
        self.assertIn("$script:AdministrativeExtractRoot = $payloadRoot", smoke)
        self.assertIn(
            'Get-ChildItem -Path $payloadRoot -Recurse -Filter '
            '"NATIVE_RELEASE_PROFILE.json"',
            smoke,
        )
        self.assertNotIn(
            'Get-ChildItem -Path $WorkRoot -Recurse -Filter '
            '"NATIVE_RELEASE_PROFILE.json"',
            smoke,
        )
        self.assertIn("administrative-extract-cleanup-proof.json", smoke)
        self.assertIn("without a pre-existing ScratchBird SAM identity", smoke)
        self.assertIn(
            "Assert-InstalledWindowsSystem -RequireGroupCreatedByThisRun",
            smoke,
        )
        self.assertIn(
            "filesystem_operations_group_preserved_after_uninstall",
            smoke,
        )
        for token in (
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
        ):
            self.assertIn(token, smoke)
        self.assertEqual(smoke.count('"WIXFAILWHENDEFERRED=1"'), 2)
        self.assertLess(
            smoke.index("msi-fault-injected-fresh-install.log"),
            smoke.index('Join-Path $WorkRoot "msi-actual-install.log"'),
        )
        self.assertLess(
            smoke.index("msi-fault-injected-uninstall.log"),
            smoke.index('Join-Path $WorkRoot "msi-actual-uninstall.log"'),
        )
        self.assertNotIn(
            'Join-Path $WorkRoot "administrative-extract"', smoke
        )
        longest_observed_relative_path = (
            "PFiles64/ScratchBird/share/scratchbird/resources/seed-packs/"
            "initial-resource-pack/resources/i18n/sbsql-language-resource-pack/"
            "resources/canonical/sbsql-dialect-baseline.schema.json"
        )
        self.assertLess(48 + 1 + len(longest_observed_relative_path), 240)
        installed_block = smoke[
            smoke.index("function Assert-InstalledWindowsSystem") :
            smoke.index("if (Test-Path $WorkRoot)")
        ]
        group_block = smoke[
            smoke.index("function Get-ExactScratchBirdGroup") :
            smoke.index("function Assert-SidNotInAnyLocalSamGroup")
        ]
        self.assertIn('$group.PSBase.Invoke("Members")', group_block)
        self.assertIn("if ($members.Count -ne 0)", group_block)
        self.assertIn(
            "$evidence.filesystem_operations_group_member_count -ne 0",
            installed_block,
        )
        self.assertIn(
            '$evidence.filesystem_operations_group_creation_policy -ne '
            '"absolute_System32_net.exe_localgroup_add_when_missing"',
            installed_block,
        )
        self.assertIn(
            "$evidence.filesystem_operations_group_created_by_this_run -isnot [bool]",
            installed_block,
        )
        self.assertIn(
            '$evidence.lifecycle_process_architecture -ne "64_bit"',
            installed_block,
        )
        self.assertIn(
            "param([switch] $RequireGroupCreatedByThisRun)",
            installed_block,
        )
        self.assertIn(
            "Fresh MSI install did not create the ScratchBird group",
            installed_block,
        )
        self.assertIn("--config-mode system-installed", installed_block)
        for config_name in installers.WINDOWS_NATIVE_CONFIGS:
            self.assertIn(f'"{config_name}"', installed_block)
        defaults_block = smoke[
            smoke.index("$configDefaults =") : smoke.index("$savedPath =")
        ]
        self.assertIn("--config-mode system-defaults", defaults_block)
        self.assertNotIn("--config-mode system-installed", defaults_block)


if __name__ == "__main__":
    unittest.main(verbosity=2)
