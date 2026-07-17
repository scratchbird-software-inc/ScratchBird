#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


REPO_ROOT: Path


class NativeQaBootstrapTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.install = self.root / "payload/opt/ScratchBird"
        (self.install / "bin").mkdir(parents=True)
        for name in ("SBsrv", "SBgate", "SBmgr", "SBParser", "SBsec", "SBsql"):
            suffix = ".exe" if os.name == "nt" else ""
            path = self.install / "bin" / f"{name}{suffix}"
            path.write_text("fixture\n", encoding="utf-8")
            path.chmod(0o700)
        for rel in (
            "share/scratchbird/resources/seed-packs/initial-resource-pack",
            "share/scratchbird/resources/policy-packs/default-local-password",
        ):
            (self.install / rel).mkdir(parents=True)
        self.cert = self.root / "operator-cert.pem"
        self.key = self.root / "operator-key.pem"
        self.cert.write_text("fixture certificate\n", encoding="utf-8")
        self.key.write_text("fixture private key\n", encoding="utf-8")
        self.key.chmod(0o600)
        helper_path = (
            REPO_ROOT
            / "project/examples/native_release_qa/prepare_native_qa_instance.py"
        )
        spec = importlib.util.spec_from_file_location(
            "sb_prepare_native_qa_instance", helper_path
        )
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader if spec else None)
        self.helper = importlib.util.module_from_spec(spec)
        assert spec is not None and spec.loader is not None
        spec.loader.exec_module(self.helper)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def command(self, instance: Path, tls_arguments: list[str] | None = None) -> list[str]:
        if tls_arguments is None:
            tls_arguments = ["--tls-cert", str(self.cert), "--tls-key", str(self.key)]
        return [
            sys.executable,
            str(
                REPO_ROOT
                / "project/examples/native_release_qa/prepare_native_qa_instance.py"
            ),
            "--install-root",
            str(self.install),
            "--config-root",
            str(REPO_ROOT / "project/config/templates"),
            "--instance-root",
            str(instance),
            "--service-identity",
            (
                self.helper.WINDOWS_SERVICE_IDENTITY
                if os.name == "nt"
                else self.helper.POSIX_SERVICE_IDENTITY
            ),
            "--service-group",
            (
                self.helper.WINDOWS_SERVICE_GROUP
                if os.name == "nt"
                else self.helper.POSIX_SERVICE_GROUP
            ),
            *tls_arguments,
            "--skip-validation",
        ]

    def run_command(self, command: list[str]):
        output = io.StringIO()
        argv = [command[1], *command[2:]]
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(sys, "argv", argv))
            if os.name != "nt":
                stack.enter_context(
                    mock.patch.object(
                        self.helper,
                        "require_posix_service_authority",
                        return_value=(
                            self.helper.POSIX_SERVICE_IDENTITY,
                            self.helper.POSIX_SERVICE_GROUP,
                        ),
                    )
                )
            if os.name == "nt":
                stack.enter_context(
                    mock.patch.object(
                        self.helper,
                        "handoff_windows_instance_acl",
                        return_value="explicit_managed_service_sid_acl",
                    )
                )
            stack.enter_context(contextlib.redirect_stdout(output))
            stack.enter_context(contextlib.redirect_stderr(output))
            try:
                returncode = self.helper.main()
            except SystemExit as error:
                returncode = int(error.code or 0)
        return subprocess.CompletedProcess(command, returncode, output.getvalue())

    def test_prepares_private_native_sbsql_instance(self) -> None:
        instance = self.root / "instance"
        result = self.run_command(self.command(instance))
        self.assertEqual(result.returncode, 0, result.stdout)
        manifest = json.loads(
            (instance / "NATIVE_QA_INSTANCE.json").read_text(encoding="utf-8")
        )
        self.assertEqual(manifest["native_parser"], "SBSQL")
        self.assertEqual(manifest["emulation_components"], "excluded")
        self.assertFalse(manifest["tls"]["private_key_embedded_in_manifest"])
        self.assertFalse(manifest["dbbt"]["key_embedded_in_manifest"])
        self.assertTrue(manifest["bootstrap"]["required_before_server_start"])
        self.assertTrue(manifest["bootstrap"]["database_must_not_exist"])
        self.assertEqual(manifest["bootstrap"]["mode"], "embedded")
        self.assertEqual(manifest["bootstrap"]["native_network_port_after_start"], 3092)
        self.assertEqual(
            manifest["bootstrap"]["os_authority_method"],
            "bootstrap.os_administrator_service_handoff",
        )
        self.assertEqual(
            manifest["bootstrap"]["os_authority_requires"],
            "root_or_administrator_only",
        )
        expected_identity = (
            self.helper.WINDOWS_SERVICE_IDENTITY
            if os.name == "nt"
            else self.helper.POSIX_SERVICE_IDENTITY
        )
        expected_group = (
            self.helper.WINDOWS_SERVICE_GROUP
            if os.name == "nt"
            else self.helper.POSIX_SERVICE_GROUP
        )
        self.assertEqual(manifest["bootstrap"]["service_identity"], expected_identity)
        self.assertEqual(manifest["bootstrap"]["service_group"], expected_group)
        bootstrap_profile = (
            instance / "config/SBbootstrap.profile"
        ).read_text(encoding="utf-8")
        self.assertIn(
            "schema_id = scratchbird.bootstrap_platform_profile.v1",
            bootstrap_profile,
        )
        self.assertIn(f"service_identity = {expected_identity}", bootstrap_profile)
        self.assertIn(f"service_group = {expected_group}", bootstrap_profile)
        self.assertNotIn("operator_required", bootstrap_profile)

        dbbt = instance / "secrets/listener-dbbt-key.hex"
        self.assertRegex(dbbt.read_text(encoding="ascii").strip(), r"^[0-9a-f]{64}$")
        server = (instance / "config/SBsrv.conf").read_text(encoding="utf-8")
        listener = (instance / "config/SBgate.conf").read_text(encoding="utf-8")
        manager = (instance / "config/SBmgr.conf").read_text(encoding="utf-8")
        self.assertIn("auto_create = false", server)
        self.assertIn("tls_required = true", server)
        self.assertIn("port = 3092", server)
        self.assertIn("port = 3092", listener)
        self.assertNotRegex(listener, r"(?m)^\s*port\s*=\s*3050\s*$")
        self.assertIn("manager.proxy.enabled = false", manager)
        self.assertIn("manager.proxy.port = 3092", manager)
        self.assertIn("manager.backend.native_port = 0", manager)
        self.assertNotRegex(
            manager,
            r"(?m)^\s*manager[.]backend[.]native_port\s*=\s*(?:3050|3090|3092|3392)\s*$",
        )
        database = (instance / "data/default.sbdb").resolve().as_posix()
        self.assertIn(f"default_path = {database}", server)
        self.assertIn(f"manager.owner.database_path = {database}", manager)

        if os.name != "nt":
            for path in (
                instance,
                instance / "config",
                instance / "runtime",
                instance / "data",
                instance / "logs",
                instance / "secrets",
            ):
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o700, str(path))
            self.assertEqual(stat.S_IMODE(dbbt.stat().st_mode), 0o600)

        repeat = self.run_command(self.command(instance))
        self.assertNotEqual(repeat.returncode, 0)
        self.assertIn("instance_root_not_empty", repeat.stdout)

    def test_rejects_config_comment_character_in_instance_path(self) -> None:
        result = self.run_command(self.command(self.root / "unsafe#instance"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("path_contains_unsafe_config_character", result.stdout)

    def test_rejects_qualified_or_domain_service_group(self) -> None:
        command = self.command(self.root / "domain-group-instance")
        group_index = command.index("--service-group") + 1
        command[group_index] = "DOMAIN\\scratchbird-qa-testers"
        result = self.run_command(command)
        self.assertNotEqual(result.returncode, 0)
        expected = (
            "windows_local_scratchbird_group_required"
            if os.name == "nt"
            else "service_group_invalid"
        )
        self.assertIn(expected, result.stdout)

    def test_windows_managed_service_identity_contract_is_exact(self) -> None:
        accepted = self.helper.validate_service_authority(
            "windows", r"NT SERVICE\scratchbird", "ScratchBird"
        )
        self.assertEqual(accepted, (r"NT SERVICE\scratchbird", "ScratchBird"))
        for identity in (
            "scratchbird",
            r".\scratchbird",
            r"MACHINE\scratchbird",
            r"DOMAIN\scratchbird",
            r"NT SERVICE\other",
            r"NT AUTHORITY\SYSTEM",
            "LocalSystem",
            "Administrator",
        ):
            with self.subTest(identity=identity), self.assertRaises(SystemExit):
                self.helper.validate_service_authority(
                    "windows", identity, "ScratchBird"
                )
        with self.assertRaises(SystemExit):
            self.helper.validate_service_authority(
                "windows", r"NT SERVICE\scratchbird", "Administrators"
            )

        source = (
            REPO_ROOT / "project/drivers/tool/cli/bootstrap_os_authority.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("use == SidTypeUser", source)
        self.assertIn('std::wstring(domain.data()) == L"NT SERVICE"', source)
        self.assertIn("*base_rid == 80", source)
        self.assertNotIn("SidTypeWellKnownGroup", source)

    def test_posix_scratchbird_identity_contract_remains_exact(self) -> None:
        self.assertEqual(
            self.helper.validate_service_authority(
                "linux", "scratchbird", "scratchbird"
            ),
            ("scratchbird", "scratchbird"),
        )
        for identity in ("root", "administrator", "qa-service"):
            with self.subTest(identity=identity), self.assertRaises(SystemExit):
                self.helper.validate_service_authority(
                    "linux", identity, "scratchbird"
                )

    @unittest.skipIf(os.name == "nt", "POSIX identity validation")
    def test_posix_environment_identity_spoof_is_rejected(self) -> None:
        operator = mock.Mock(
            pw_name="operator",
            pw_uid=4242,
            pw_gid=4242,
        )
        with (
            mock.patch.dict(
                os.environ,
                {"USER": "scratchbird", "LOGNAME": "scratchbird"},
                clear=False,
            ),
            mock.patch.object(self.helper.os, "geteuid", return_value=4242),
            mock.patch.object(self.helper.os, "getegid", return_value=4242),
            mock.patch.object(self.helper.pwd, "getpwuid", return_value=operator),
            self.assertRaises(SystemExit),
        ):
            self.helper.require_posix_service_authority()

        helper_source = (
            REPO_ROOT
            / "project/examples/native_release_qa/prepare_native_qa_instance.py"
        ).read_text(encoding="utf-8")
        self.assertIn("pwd.getpwuid(effective_uid)", helper_source)
        self.assertNotIn("getpass.getuser", helper_source)

    @unittest.skipIf(os.name == "nt", "POSIX identity validation")
    def test_posix_numeric_service_membership_must_be_exact(self) -> None:
        service_user = mock.Mock(
            pw_name="scratchbird",
            pw_uid=4242,
            pw_gid=4242,
        )
        service_group = mock.Mock(
            gr_name="scratchbird",
            gr_gid=4242,
            gr_mem=[],
        )
        admin_group = mock.Mock(
            gr_name="admin",
            gr_gid=80,
            gr_mem=["scratchbird"],
        )

        def identity_patches(groups: list[int], records: list[object]):
            return (
                mock.patch.object(self.helper.os, "geteuid", return_value=4242),
                mock.patch.object(self.helper.os, "getegid", return_value=4242),
                mock.patch.object(self.helper.os, "getgroups", return_value=groups),
                mock.patch.object(
                    self.helper.pwd, "getpwuid", return_value=service_user
                ),
                mock.patch.object(
                    self.helper.pwd, "getpwnam", return_value=service_user
                ),
                mock.patch.object(
                    self.helper.pwd, "getpwall", return_value=[service_user]
                ),
                mock.patch.object(
                    self.helper.grp, "getgrgid", return_value=service_group
                ),
                mock.patch.object(
                    self.helper.grp, "getgrnam", return_value=service_group
                ),
                mock.patch.object(
                    self.helper.grp, "getgrall", return_value=records
                ),
            )

        with contextlib.ExitStack() as stack:
            for patcher in identity_patches([4242], [service_group]):
                stack.enter_context(patcher)
            self.assertEqual(
                self.helper.require_posix_service_authority(),
                ("scratchbird", "scratchbird"),
            )

        with contextlib.ExitStack() as stack:
            for patcher in identity_patches(
                [4242, 80], [service_group, admin_group]
            ):
                stack.enter_context(patcher)
            with self.assertRaises(SystemExit):
                self.helper.require_posix_service_authority()

        with contextlib.ExitStack() as stack:
            for patcher in identity_patches(
                [4242, 12, 61], [service_group]
            ):
                stack.enter_context(patcher)
            stack.enter_context(
                mock.patch.object(self.helper.sys, "platform", "darwin")
            )
            self.assertEqual(
                self.helper.require_posix_service_authority(),
                ("scratchbird", "scratchbird"),
            )

        with contextlib.ExitStack() as stack:
            for patcher in identity_patches(
                [4242, 12, 61, 80], [service_group]
            ):
                stack.enter_context(patcher)
            stack.enter_context(
                mock.patch.object(self.helper.sys, "platform", "darwin")
            )
            with self.assertRaises(SystemExit):
                self.helper.require_posix_service_authority()

    def test_os_service_identity_never_becomes_database_principal(self) -> None:
        authority_source = (
            REPO_ROOT / "project/drivers/tool/cli/bootstrap_os_authority.cpp"
        ).read_text(encoding="utf-8")
        for token in (
            "NetLocalGroupEnum",
            "NetLocalGroupGetMembers",
            "NetApiBufferFree",
            "BootstrapWindowsServiceSidGroupInventoryIsLeastAuthority",
            "service_identity_explicit_local_group_membership_forbidden_or_",
        ):
            self.assertIn(token, authority_source)
        self.assertNotIn("LookupIntendedLocalGroup", authority_source)
        self.assertNotIn("configured_local_group_membership_required", authority_source)
        self.assertNotIn("root_and_configured_group_membership_required", authority_source)
        profile_source = (
            REPO_ROOT / "project/config/templates/SBbootstrap.profile"
        ).read_text(encoding="utf-8")
        self.assertIn(
            "Root/Administrator is the sole create-time OS authorization gate",
            profile_source,
        )
        cli_cmake = (
            REPO_ROOT / "project/drivers/tool/cli/CMakeLists.txt"
        ).read_text(encoding="utf-8")
        self.assertEqual(cli_cmake.count("PRIVATE advapi32 netapi32"), 2)

        security_source = (
            REPO_ROOT / "project/drivers/tool/cli/sb_security.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn(
            "request.bootstrap_principal_name = g_config.username;",
            security_source,
        )
        self.assertNotIn(
            "request.bootstrap_principal_name = os_authority.service_identity;",
            security_source,
        )
        self.assertNotIn(
            "request.bootstrap_principal_name = loaded_profile.profile.service_identity;",
            security_source,
        )

    def test_bootstrap_secret_surface_is_not_argv_env_or_log_authority(self) -> None:
        source = (
            REPO_ROOT / "project/drivers/tool/cli/sb_security.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("Bootstrap passwords are forbidden in command-line arguments", source)
        self.assertIn("--password-stdin", source)
        self.assertNotIn("std::getenv", source)
        self.assertNotRegex(source, r"log(?:Verbose)?\([^\n]*credential_fingerprint")
        self.assertNotRegex(source, r"printError\([^\n]*(?:verifier|salt)=")
        readme = (
            REPO_ROOT / "project/examples/native_release_qa/README.md"
        ).read_text(encoding="utf-8")
        self.assertNotRegex(readme, r"--password(?:=|\s+)[^s]")

    @unittest.skipUnless(shutil.which("openssl"), "openssl is not installed")
    def test_generated_tls_key_is_never_left_public(self) -> None:
        instance = self.root / "generated-tls-instance"
        result = self.run_command(
            self.command(instance, ["--generate-self-signed-tls"])
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue((instance / "tls/server-cert.pem").is_file())
        key = instance / "tls/server-key.pem"
        self.assertTrue(key.is_file())
        if os.name != "nt":
            self.assertEqual(stat.S_IMODE(key.stat().st_mode), 0o600)

    def test_macos_portable_payload_is_foreground_only(self) -> None:
        module_path = REPO_ROOT / "project/tools/installers/build_installers.py"
        spec = importlib.util.spec_from_file_location("sb_build_installers", module_path)
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader if spec else None)
        module = importlib.util.module_from_spec(spec)
        assert spec is not None and spec.loader is not None
        spec.loader.exec_module(module)
        artifact = self.root / "macos-artifact"
        for directory in ("bin", "lib", "share", "etc"):
            (artifact / directory).mkdir(parents=True)
        payload = self.root / "macos-payload"
        module.stage_install_tree(
            artifact,
            payload,
            "macos",
            "0.0.0-test",
            "portable-foreground-test",
        )
        self.assertFalse((payload / "Library/LaunchDaemons").exists())
        self.assertFalse(
            (
                payload
                / "opt/ScratchBird/share/scratchbird/release/"
                "MACOS_LAUNCHD_MANIFEST.json"
            ).exists()
        )
        self.assertTrue(
            (
                payload
                / "opt/ScratchBird/share/scratchbird/release/"
                "MACOS_SUPPORT_MATRIX.json"
            ).is_file()
        )
        support = json.loads(
            (
                payload
                / "opt/ScratchBird/share/scratchbird/release/"
                "MACOS_SUPPORT_MATRIX.json"
            ).read_text(encoding="utf-8")
        )
        self.assertEqual(
            support["service_packaging"]["portable_tar"],
            "foreground_only_no_launchd_definitions",
        )
        smoke = (
            REPO_ROOT / "project/tools/installers/smoke_install_macos.sh"
        ).read_text(encoding="utf-8")
        self.assertIn("portable_launchd_manifest_forbidden", smoke)
        self.assertIn("portable_launchd_root_forbidden", smoke)
        self.assertIn("execution_mode=foreground_only", smoke)
        canonical_work_root = 'work_root="$(cd "$work_root" && pwd -P)"'
        self.assertIn(canonical_work_root, smoke)
        self.assertLess(
            smoke.index(canonical_work_root),
            smoke.index('--root "$fresh_root"'),
        )
        self.assertIn(
            '"$runtime_root" --config-root "$config_root" '
            "--config-mode system-installed",
            smoke,
        )
        self.assertNotIn(
            '"$payload_root" --config-root "$config_root" '
            "--config-mode system-installed",
            smoke,
        )

    @unittest.skipUnless(
        os.name != "nt" and shutil.which("bash"), "bash is not installed"
    )
    def test_macos_pkg_smoke_accepts_both_scripts_representations(self) -> None:
        fake_bin = self.root / "fake-macos-tools"
        fake_bin.mkdir()
        fake_uname = fake_bin / "uname"
        fake_uname.write_text(
            "#!/usr/bin/env bash\nset -euo pipefail\necho Darwin\n",
            encoding="utf-8",
        )
        fake_pkgutil = fake_bin / "pkgutil"
        fake_pkgutil.write_text(
            """#!/usr/bin/env bash
set -euo pipefail
test "${1:-}" = --expand
expanded="${3:?expanded package root is required}"
mkdir -p "$expanded"
: > "$expanded/Payload"
case "${SB_TEST_PKG_SCRIPTS_REPRESENTATION:?}" in
  directory)
    mkdir -p "$expanded/Scripts"
    printf '%s\\n' '#!/bin/sh' \\
      '/opt/ScratchBird/libexec/scratchbird-macos-system-install post-install' \\
      > "$expanded/Scripts/postinstall"
    ;;
  archive)
    : > "$expanded/Scripts"
    printf '%s\\n' '#!/bin/sh' \\
      '/opt/ScratchBird/libexec/scratchbird-macos-system-install post-install' \\
      > "$expanded/Scripts.postinstall"
    ;;
  *)
    exit 64
    ;;
esac
""",
            encoding="utf-8",
        )
        fake_ditto = fake_bin / "ditto"
        fake_ditto.write_text(
            """#!/usr/bin/env bash
set -euo pipefail
test "${1:-}" = -x
source_path="${2:?source is required}"
target_path="${3:?target is required}"
mkdir -p "$target_path"
if [[ "$(basename "$source_path")" = Scripts ]]; then
  cp "$source_path.postinstall" "$target_path/postinstall"
fi
""",
            encoding="utf-8",
        )
        for executable in (fake_uname, fake_pkgutil, fake_ditto):
            executable.chmod(0o755)

        package = self.root / "scratchbird-fixture.pkg"
        package.touch()
        smoke = REPO_ROOT / "project/tools/installers/smoke_install_macos.sh"
        for representation in ("directory", "archive"):
            work_root = self.root / f"macos-pkg-smoke-{representation}"
            environment = os.environ.copy()
            environment["PATH"] = f"{fake_bin}{os.pathsep}{environment['PATH']}"
            environment["SB_TEST_PKG_SCRIPTS_REPRESENTATION"] = representation
            result = subprocess.run(
                ["bash", str(smoke), str(package), str(work_root)],
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn("smoke_install_macos=fail:runtime_bin_missing", result.stdout)
            self.assertNotIn("pkg_scripts_missing", result.stdout)
            self.assertNotIn("pkg_postinstall_missing", result.stdout)
            postinstall = (
                work_root / "pkg-expanded/Scripts/postinstall"
                if representation == "directory"
                else work_root / "pkg-scripts/postinstall"
            )
            self.assertTrue(postinstall.is_file(), result.stdout)

    def test_linux_system_payload_materializes_runtime_and_identity(self) -> None:
        builder_path = REPO_ROOT / "project/tools/installers/build_installers.py"
        builder_spec = importlib.util.spec_from_file_location(
            "sb_build_installers_linux", builder_path
        )
        self.assertIsNotNone(builder_spec)
        self.assertIsNotNone(builder_spec.loader if builder_spec else None)
        builder = importlib.util.module_from_spec(builder_spec)
        assert builder_spec is not None and builder_spec.loader is not None
        builder_spec.loader.exec_module(builder)

        smoke_path = (
            REPO_ROOT / "project/tools/installers/smoke_install_linux_system.py"
        )
        smoke_spec = importlib.util.spec_from_file_location(
            "sb_smoke_install_linux_system", smoke_path
        )
        self.assertIsNotNone(smoke_spec)
        self.assertIsNotNone(smoke_spec.loader if smoke_spec else None)
        smoke = importlib.util.module_from_spec(smoke_spec)
        assert smoke_spec is not None and smoke_spec.loader is not None
        smoke_spec.loader.exec_module(smoke)

        portable = self.root / "linux-portable"
        runtime = portable / "opt/ScratchBird"
        for directory in ("bin", "lib", "share/scratchbird/resources"):
            (runtime / directory).mkdir(parents=True)
        config = portable / "etc/scratchbird"
        config.mkdir(parents=True)
        templates = REPO_ROOT / "project/config/templates"
        for name in self.helper.CONFIG_NAMES:
            shutil.copy2(templates / name, config / name)

        system = self.root / "linux-system"
        builder.stage_linux_system_install_tree(
            portable,
            system,
            "0.0.0-test",
            "linux-materialization-test",
        )
        for relative, tokens in smoke.SYSTEM_CONFIG_TOKENS.items():
            text = (system / relative).read_text(encoding="utf-8")
            for token in tokens:
                self.assertIn(token, text, f"{relative}: {token}")
            for token in smoke.FORBIDDEN_SYSTEM_CONFIG_TOKENS:
                self.assertNotIn(token, text, f"{relative}: {token}")

        profile = json.loads(
            (
                system
                / "opt/ScratchBird/share/scratchbird/release/"
                "LINUX_SYSTEM_INSTALL_PROFILE.json"
            ).read_text(encoding="utf-8")
        )
        self.assertEqual(
            profile["os_identity"]["service_authority_scope"],
            "filesystem_directory_and_process_execution_only_"
            "no_database_or_security_authority",
        )
        self.assertIn(
            "only_scratchbird_effective_group",
            profile["os_identity"]["existing_identity_validation"],
        )
        self.assertEqual(
            profile["os_identity"]["create_time_os_authorization"], "root_only"
        )
        self.assertEqual(
            profile["os_identity"]["human_service_group_membership_mutation"],
            "forbidden",
        )
        lifecycle_helper = (
            system / "usr/lib/scratchbird/scratchbird-system-install"
        ).read_text(encoding="utf-8")
        self.assertIn(
            "filesystem_directory_and_process_execution_only_"
            "no_database_or_security_authority",
            lifecycle_helper,
        )
        evidence_root = self.root / "linux-evidence"
        evidence_root.mkdir()
        evidence = json.loads(
            builder.write_linux_system_package_evidence(
                evidence_root, "0.0.0-test", "linux-materialization-test"
            ).read_text(encoding="utf-8")
        )
        self.assertEqual(
            evidence["os_identity"]["service_authority_scope"],
            "filesystem_directory_and_process_execution_only_"
            "no_database_or_security_authority",
        )
        self.assertEqual(
            evidence["os_identity"]["service_effective_group_policy"],
            "only_scratchbird_effective_group",
        )
        self.assertEqual(
            evidence["os_identity"]["create_time_os_authorization"], "root_only"
        )
        self.assertFalse(
            evidence["os_identity"]["human_service_group_membership_mutation"]
        )

        portable_bootstrap = (
            portable / "etc/scratchbird/SBbootstrap.profile"
        ).read_text(encoding="utf-8")
        self.assertIn("platform = operator_required", portable_bootstrap)
        portable_parser = (
            portable / "etc/scratchbird/SBParser.conf"
        ).read_text(encoding="utf-8")
        self.assertIn("parser.worker_binary = bin/SBParser", portable_parser)

    def test_system_config_verifier_uses_platform_installer_contracts(self) -> None:
        builder_path = REPO_ROOT / "project/tools/installers/build_installers.py"
        builder_spec = importlib.util.spec_from_file_location(
            "sb_build_installers_system_contract_test", builder_path
        )
        self.assertIsNotNone(builder_spec)
        self.assertIsNotNone(builder_spec.loader if builder_spec else None)
        builder = importlib.util.module_from_spec(builder_spec)
        assert builder_spec is not None and builder_spec.loader is not None
        builder_spec.loader.exec_module(builder)

        release_tools = REPO_ROOT / "project/tools/release"
        sys.path.insert(0, str(release_tools))
        try:
            verifier_path = release_tools / "verify_native_installed_payload.py"
            verifier_spec = importlib.util.spec_from_file_location(
                "sb_verify_native_system_contract_test", verifier_path
            )
            self.assertIsNotNone(verifier_spec)
            self.assertIsNotNone(verifier_spec.loader if verifier_spec else None)
            verifier = importlib.util.module_from_spec(verifier_spec)
            assert verifier_spec is not None and verifier_spec.loader is not None
            verifier_spec.loader.exec_module(verifier)
        finally:
            sys.path.remove(str(release_tools))

        templates = REPO_ROOT / "project/config/templates"

        def portable_fixture(name: str) -> Path:
            portable = self.root / f"{name}-portable"
            (portable / "opt/ScratchBird/bin").mkdir(parents=True)
            (portable / "opt/ScratchBird/lib").mkdir()
            (portable / "opt/ScratchBird/share").mkdir()
            config = portable / "etc/scratchbird"
            config.mkdir(parents=True)
            for config_name in self.helper.CONFIG_NAMES:
                shutil.copy2(templates / config_name, config / config_name)
            return portable

        linux_system = self.root / "linux-system-contract"
        builder.stage_linux_system_install_tree(
            portable_fixture("linux"), linux_system, "0.0.0-test", "contract-test"
        )
        verifier.require_system_configs(
            linux_system / "etc/scratchbird",
            "linux",
            linux_system / "opt/ScratchBird",
            "system-installed",
        )

        macos_system = self.root / "macos-system-contract"
        builder.stage_macos_system_install_tree(
            portable_fixture("macos"), macos_system, "0.0.0-test", "contract-test"
        )
        macos_config = (
            macos_system
            / "opt/ScratchBird/share/scratchbird/config-defaults"
        )
        verifier.require_system_configs(
            macos_config,
            "macos",
            macos_system / "opt/ScratchBird",
            "system-installed",
        )

        windows_defaults = self.root / "windows-system-contract"
        builder.stage_windows_system_install_tree(
            portable_fixture("windows"),
            windows_defaults,
            "0.0.0-test",
            "contract-test",
        )
        windows_defaults_config = (
            windows_defaults / "share/scratchbird/config-defaults"
        )
        verifier.require_system_configs(
            windows_defaults_config,
            "windows",
            windows_defaults,
            "system-defaults",
        )
        defaults_text = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted(windows_defaults_config.iterdir())
        )
        self.assertIn("@SCRATCHBIRD_INSTALL_ROOT@/bin/SBParser.exe", defaults_text)
        self.assertIn("@SCRATCHBIRD_STATE_ROOT@/data/default.sbdb", defaults_text)

        windows_runtime = self.root / "windows-installed/Program Files/ScratchBird"
        windows_state = self.root / "windows-installed/ProgramData/ScratchBird"
        windows_config = windows_state / "config"
        windows_config.mkdir(parents=True)
        install_value = windows_runtime.as_posix()
        state_value = windows_state.as_posix()
        for source in windows_defaults_config.iterdir():
            text = source.read_text(encoding="utf-8")
            text = text.replace("@SCRATCHBIRD_INSTALL_ROOT@", install_value)
            text = text.replace("@SCRATCHBIRD_STATE_ROOT@", state_value)
            (windows_config / source.name).write_text(text, encoding="utf-8")
        verifier.require_system_configs(
            windows_config,
            "windows",
            windows_runtime,
            "system-installed",
        )
        windows_parser = (windows_config / "SBParser.conf").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            f"parser.worker_binary = {install_value}/bin/SBParser.exe",
            windows_parser,
        )

        parser_path = macos_config / "SBParser.conf"
        valid_parser = parser_path.read_text(encoding="utf-8")
        invalid_variants = {
            "portable_parser_path": valid_parser.replace(
                "/opt/ScratchBird/bin/SBParser", "bin/SBParser", 1
            ),
            "compatibility_assignment": (
                valid_parser + "\nparser.compatibility.enabled = true\n"
            ),
            "emulation_assignment": (
                valid_parser + "\nparser.emulation.enabled = true\n"
            ),
            "unresolved_system_placeholder": (
                valid_parser + "\nparser.state_root = @SCRATCHBIRD_STATE_ROOT@\n"
            ),
            "unknown_system_placeholder": (
                valid_parser + "\nparser.state_root = @OTHER_ROOT@\n"
            ),
            "comment_spoofed_expected_assignment": valid_parser.replace(
                "parser.worker_binary = /opt/ScratchBird/bin/SBParser",
                "# parser.worker_binary = /opt/ScratchBird/bin/SBParser\n"
                "parser.worker_binary = /tmp/untrusted-parser",
                1,
            ),
            "conflicting_duplicate_assignment": (
                valid_parser + "\nparser.worker_binary = /tmp/untrusted-parser\n"
            ),
        }
        for label, invalid_text in invalid_variants.items():
            with self.subTest(rejection=label):
                parser_path.write_text(invalid_text, encoding="utf-8")
                with (
                    contextlib.redirect_stderr(io.StringIO()),
                    self.assertRaises(SystemExit),
                ):
                    verifier.require_system_configs(
                        macos_config,
                        "macos",
                        macos_system / "opt/ScratchBird",
                        "system-installed",
                    )
        parser_path.write_text(valid_parser, encoding="utf-8")

        server_path = macos_config / "SBsrv.conf"
        valid_server = server_path.read_text(encoding="utf-8")
        server_invalid_variants = {
            "expected_assignment_in_wrong_section": valid_server.replace(
                "data_dir = /var/run/scratchbird/sb_server",
                "# data_dir = /var/run/scratchbird/sb_server",
                1,
            )
            + "\n[server.metrics]\n"
            "data_dir = /var/run/scratchbird/sb_server\n",
            "protected_assignments_swapped_between_sections": valid_server.replace(
                "control_dir = /var/run/scratchbird/sb_server/control",
                "control_dir = @SWAP@",
                1,
            )
            .replace(
                "control_dir = /var/run/scratchbird/listener/control",
                "control_dir = /var/run/scratchbird/sb_server/control",
                1,
            )
            .replace(
                "control_dir = @SWAP@",
                "control_dir = /var/run/scratchbird/listener/control",
                1,
            ),
            "database_auto_create_enabled": valid_server.replace(
                "auto_create = false",
                "auto_create = true",
                1,
            ),
            "database_auto_create_in_wrong_section": valid_server.replace(
                "auto_create = false",
                "# auto_create = false",
                1,
            )
            + "\n[server.metrics]\n"
            "auto_create = false\n",
            "database_auto_create_conflicting_duplicate": (
                valid_server
                + "\n[server.database]\n"
                "auto_create = true\n"
            ),
        }
        for label, invalid_text in server_invalid_variants.items():
            with self.subTest(rejection=label):
                server_path.write_text(invalid_text, encoding="utf-8")
                with (
                    contextlib.redirect_stderr(io.StringIO()),
                    self.assertRaises(SystemExit),
                ):
                    verifier.require_system_configs(
                        macos_config,
                        "macos",
                        macos_system / "opt/ScratchBird",
                        "system-installed",
                    )
        server_path.write_text(
            valid_server.replace("port = 3092", "port = 3050", 1),
            encoding="utf-8",
        )
        with (
            contextlib.redirect_stderr(io.StringIO()),
            self.assertRaises(SystemExit),
        ):
            verifier.require_system_configs(
                macos_config,
                "macos",
                macos_system / "opt/ScratchBird",
                "system-installed",
            )


def main() -> int:
    global REPO_ROOT
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    args, remaining = parser.parse_known_args()
    REPO_ROOT = args.repo_root.resolve()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(NativeQaBootstrapTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
