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
import zipfile


REPO_ROOT = Path(__file__).resolve().parents[4]
BUILDER_PATH = REPO_ROOT / "project" / "tools" / "installers" / "build_installers.py"
ARTIFACT_VERIFIER_PATH = (
    REPO_ROOT / "project" / "tools" / "installers" / "verify_installer_artifacts.py"
)
SPEC = importlib.util.spec_from_file_location("scratchbird_build_installers", BUILDER_PATH)
assert SPEC is not None and SPEC.loader is not None
installers = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(installers)
ARTIFACT_VERIFIER_SPEC = importlib.util.spec_from_file_location(
    "scratchbird_verify_installer_artifacts", ARTIFACT_VERIFIER_PATH
)
assert ARTIFACT_VERIFIER_SPEC is not None and ARTIFACT_VERIFIER_SPEC.loader is not None
artifact_verifier = importlib.util.module_from_spec(ARTIFACT_VERIFIER_SPEC)
ARTIFACT_VERIFIER_SPEC.loader.exec_module(artifact_verifier)
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
    def write_lifecycle_wix_pdb(
        self,
        path: Path,
        *,
        omit_action: str | None = None,
        post_action_type: int = 11265,
        post_action_sequence: int = 6599,
        powershell_folder: str = "System64Folder",
    ) -> None:
        namespace = "http://wixtoolset.org/schemas/v4/windowsinstallerdata"
        ET.register_namespace("", namespace)
        root = ET.Element(f"{{{namespace}}}windowsInstallerData")

        def add_table(name: str, rows: list[list[str]]) -> None:
            table = ET.SubElement(root, f"{{{namespace}}}table", {"name": name})
            for values in rows:
                row = ET.SubElement(table, f"{{{namespace}}}row")
                for value in values:
                    field = ET.SubElement(row, f"{{{namespace}}}field")
                    field.text = value

        post_command = (
            f'"[{powershell_folder}]WindowsPowerShell\\v1.0\\powershell.exe" '
            '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass '
            '-File "[INSTALLFOLDER]libexec\\scratchbird-windows-system-install.ps1" '
            '-Action PostInstall -InstallRoot "[INSTALLFOLDER]." '
            '-StateRoot "[CommonAppDataFolder]ScratchBird"'
        )
        pre_command = post_command.replace("PostInstall", "PreRemove")
        custom_actions = [
            [
                "SetScratchBirdPostInstall",
                "51",
                "ScratchBirdPostInstall",
                post_command,
            ],
            [
                "SetScratchBirdPreRemove",
                "51",
                "ScratchBirdPreRemove",
                pre_command,
            ],
            [
                "ScratchBirdPostInstall",
                str(post_action_type),
                "Wix4UtilCA_X64",
                "WixQuietExec",
            ],
            [
                "ScratchBirdPreRemove",
                "11265",
                "Wix4UtilCA_X64",
                "WixQuietExec",
            ],
        ]
        if omit_action is not None:
            custom_actions = [row for row in custom_actions if row[0] != omit_action]
        add_table("CustomAction", custom_actions)
        post_condition = 'NOT (REMOVE~="ALL")'
        pre_condition = 'REMOVE~="ALL" AND NOT UPGRADINGPRODUCTCODE'
        add_table(
            "InstallExecuteSequence",
            [
                ["InstallInitialize", "", "1500"],
                ["SetScratchBirdPreRemove", pre_condition, "3498"],
                ["ScratchBirdPreRemove", pre_condition, "3499"],
                ["RemoveFiles", "", "3500"],
                ["InstallFiles", "", "4000"],
                ["SetScratchBirdPostInstall", post_condition, "6598"],
                [
                    "ScratchBirdPostInstall",
                    post_condition,
                    str(post_action_sequence),
                ],
                ["InstallFinalize", "", "6600"],
            ],
        )
        add_table("AdminExecuteSequence", [])
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr(
                "wix-wid.xml", ET.tostring(root, encoding="utf-8")
            )

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
            self.assertNotIn("server.listener.native", all_text)
            self.assertNotRegex(
                all_text, r"(?m)^\s*\[server[.]listener[.]profile[.]"
            )
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
                profile["create_time_os_authorization"], "administrator_only"
            )
            self.assertEqual(
                profile["human_service_group_membership_mutation"], "forbidden"
            )
            self.assertEqual(
                profile["os_identity"]["group_purpose"],
                "filesystem_operations_only_no_database_or_security_authority",
            )
            self.assertTrue(profile["service"]["create_if_missing"])
            self.assertEqual(
                profile["service"]["creation_mechanism"],
                "elevated_deferred_msi_lifecycle_helper_Ensure-SBsrvService",
            )
            self.assertEqual(
                profile["service"]["fresh_install_failure_service_rollback"],
                "remove_service_created_by_this_install_attempt",
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
            self.assertIn(
                '<CustomActionRef Id="ScratchBirdPostInstall" />', main
            )
            self.assertIn(
                '<CustomActionRef Id="ScratchBirdPreRemove" />', main
            )
            self.assertNotIn("SB_INSTALLER_USER", lifecycle)
            self.assertNotIn("InstallerUser", lifecycle)
            self.assertIn('Execute="deferred"', lifecycle)
            self.assertIn('Impersonate="no"', lifecycle)
            self.assertIn(
                '[System64Folder]WindowsPowerShell\\v1.0\\powershell.exe',
                lifecycle,
            )
            self.assertNotIn(
                '[SystemFolder]WindowsPowerShell\\v1.0\\powershell.exe',
                lifecycle,
            )
            self.assertIn(
                '-InstallRoot &quot;[INSTALLFOLDER].&quot;', lifecycle
            )
            self.assertNotIn(
                '-InstallRoot &quot;[INSTALLFOLDER]&quot;', lifecycle
            )
            self.assertIn("NOT UPGRADINGPRODUCTCODE", lifecycle)
            self.assertNotIn("@SCRATCHBIRD_VERSION@", lifecycle)
            self.assertEqual(lifecycle.count("<Fragment"), 1)
            self.assertIn("<InstallExecuteSequence>", lifecycle)
            self.assertIn('Action="ScratchBirdPostInstall"', lifecycle)
            self.assertIn('Action="ScratchBirdPreRemove"', lifecycle)
            ET.fromstring(main)
            ET.fromstring(lifecycle)

    def test_material_wix_pdb_lifecycle_gate(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            msi = root / "scratchbird-windows-1.2.3.msi"
            msi.write_bytes(b"fixture-msi")
            pdb = msi.with_suffix(".wixpdb")

            self.write_lifecycle_wix_pdb(pdb)
            artifact_verifier.verify_windows_material_msi_lifecycle(
                root, msi.name
            )

            with self.subTest("missing custom action"):
                self.write_lifecycle_wix_pdb(
                    pdb, omit_action="ScratchBirdPostInstall"
                )
                with self.assertRaises(SystemExit):
                    artifact_verifier.verify_windows_material_msi_lifecycle(
                        root, msi.name
                    )
            with self.subTest("incorrect custom action type"):
                self.write_lifecycle_wix_pdb(pdb, post_action_type=1)
                with self.assertRaises(SystemExit):
                    artifact_verifier.verify_windows_material_msi_lifecycle(
                        root, msi.name
                    )
            with self.subTest("incorrect post-install ordering"):
                self.write_lifecycle_wix_pdb(pdb, post_action_sequence=6601)
                with self.assertRaises(SystemExit):
                    artifact_verifier.verify_windows_material_msi_lifecycle(
                        root, msi.name
                    )
            with self.subTest("32-bit PowerShell is forbidden"):
                self.write_lifecycle_wix_pdb(
                    pdb, powershell_folder="SystemFolder"
                )
                with self.assertRaises(SystemExit):
                    artifact_verifier.verify_windows_material_msi_lifecycle(
                        root, msi.name
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
            self.assertTrue(
                (output / "scratchbird-windows-1.2.3-nightly.wixpdb").is_file()
            )
            artifact_verifier.verify_windows_material_msi_lifecycle(
                output,
                "scratchbird-windows-1.2.3-nightly.msi",
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
        self.assertIn(
            "service_local_sam_group_membership = $false", lifecycle
        )

    def test_virtual_service_account_is_created_atomically_with_null_password_pointer(self) -> None:
        lifecycle = (
            REPO_ROOT
            / "project/tools/installers/windows/"
            "scratchbird-windows-system-install.ps1"
        ).read_text(encoding="utf-8")
        self.assertIn("function New-ManagedVirtualService", lifecycle)
        self.assertIn("OpenSCManagerW", lifecycle)
        self.assertIn('"ServicesActive"', lifecycle)
        self.assertIn("PowerShell null-marshalling behavior", lifecycle)
        self.assertIn("CreateServiceW", lifecycle)
        self.assertIn("IntPtr lpPassword", lifecycle)
        self.assertIn("true NULL password pointer", lifecycle)
        self.assertIn("NATIVE_EXIT_$nativeExitCode", lifecycle)
        self.assertIn("[IntPtr]::Zero", lifecycle)
        self.assertIn("[uint32]0x00000010", lifecycle)
        self.assertIn("[uint32]0x00000003", lifecycle)
        self.assertIn("[uint32]0x00000001", lifecycle)
        self.assertIn("Get-ExpectedServiceCommand", lifecycle)
        self.assertIn("CREATE_SERVICE_ERROR_$lastError", lifecycle)
        self.assertIn(
            "New-ManagedVirtualService\n    $script:ServiceCreatedByThisRun",
            lifecycle,
        )
        self.assertIn('SERVICE_IDENTITY.ASSERT_EXISTING', lifecycle)
        self.assertNotIn("ChangeServiceConfigW", lifecycle)
        self.assertNotIn("OpenServiceW", lifecycle)
        self.assertNotIn('SERVICE_IDENTITY.CONFIGURE_ACCOUNT', lifecycle)
        self.assertNotIn('@("create", $ServiceName', lifecycle)
        self.assertNotIn('"obj= $ServiceAccount"', lifecycle)
        self.assertNotIn("password=", lifecycle)


if __name__ == "__main__":
    unittest.main(verbosity=2)
