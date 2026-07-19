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
        self.assertEqual(
            manifest["dbbt"]["source"], "local_qa_keyring_environment_bridge"
        )
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
        self.assertIn("[server.listener.profile.sbsql]", server)
        self.assertIn("enabled = true", server)
        self.assertIn("protocol_family = sbsql", server)
        self.assertIn("profile_id = default", server)
        self.assertIn("parser_package = SBParser", server)
        self.assertIn("parser_package_uuid = parser.native.scratchbird", server)
        self.assertIn("dialect_profile_uuid = sbsql_v3", server)
        self.assertIn("bundle_contract_id = sbp_sbsql@1", server)
        self.assertIn("parser_api_major = 1", server)
        self.assertIn("tls_required = true", server)
        self.assertIn(f"tls_cert_file = {self.cert.resolve()}", server)
        self.assertIn(f"tls_key_file = {self.key.resolve()}", server)
        self.assertIn("port = 3092", server)
        self.assertNotIn("server.listener.native", server)
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
        self.assertIn(
            "add_library(sb_cli_first_principal_bootstrap STATIC", cli_cmake
        )
        self.assertIn(
            "target_link_libraries(sb_cli_first_principal_bootstrap PUBLIC advapi32 netapi32)",
            cli_cmake,
        )
        self.assertIn(
            "target_link_libraries(sb_isql PRIVATE sb_cli_first_principal_bootstrap)",
            cli_cmake,
        )
        self.assertIn(
            "target_link_libraries(sb_security PRIVATE OpenSSL::Crypto sb_cli_first_principal_bootstrap)",
            cli_cmake,
        )

        bootstrap_source = (
            REPO_ROOT / "project/drivers/tool/cli/first_principal_bootstrap.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn(
            "create_request.bootstrap_principal_name = request.principal_name;",
            bootstrap_source,
        )
        self.assertNotIn(
            "create_request.bootstrap_principal_name = os_authority.service_identity;",
            bootstrap_source,
        )
        self.assertNotIn(
            "create_request.bootstrap_principal_name = loaded_profile.profile.service_identity;",
            bootstrap_source,
        )
        for tool_name in ("sb_isql.cpp", "sb_security.cpp"):
            tool_source = (
                REPO_ROOT / "project/drivers/tool/cli" / tool_name
            ).read_text(encoding="utf-8")
            self.assertIn("RunFirstPrincipalBootstrapCli", tool_source)
            self.assertNotIn("CreateDatabaseFile", tool_source)

    def test_bootstrap_secret_surface_is_not_argv_env_or_log_authority(self) -> None:
        source = (
            REPO_ROOT / "project/drivers/tool/cli/first_principal_bootstrap.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("Bootstrap passwords are forbidden in command-line arguments", source)
        self.assertIn("--password-stdin", source)
        self.assertNotIn("std::getenv", source)
        self.assertNotRegex(source, r"(?:output|error)\s*<<[^\n]*credential_fingerprint")
        self.assertNotRegex(source, r"(?:output|error)\s*<<[^\n]*(?:verifier|salt)=")
        for tool_name in ("sb_isql.cpp", "sb_security.cpp"):
            tool_source = (
                REPO_ROOT / "project/drivers/tool/cli" / tool_name
            ).read_text(encoding="utf-8")
            self.assertIn("RunFirstPrincipalBootstrapCli", tool_source)
            self.assertNotIn("CreateDatabaseFile", tool_source)
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

    @unittest.skipIf(os.name == "nt", "requires a POSIX shell fixture")
    def test_macos_pkg_smoke_accepts_expanded_scripts_directory(self) -> None:
        """pkgutil may materialize Scripts as a directory instead of an archive."""

        smoke = REPO_ROOT / "project/tools/installers/smoke_install_macos.sh"
        tools = self.root / "macos-pkg-smoke-tools"
        tools.mkdir()

        def write_tool(name: str, content: str) -> None:
            path = tools / name
            path.write_text(content, encoding="utf-8")
            path.chmod(0o755)

        write_tool(
            "uname",
            "#!/usr/bin/env bash\nprintf 'Darwin\\n'\n",
        )
        write_tool(
            "pkgutil",
            """#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == "--expand" ]]; then
  expanded="${3:?missing expansion root}"
  component="$expanded/com.scratchbird.cde.pkg"
  mkdir -p "$component/Scripts"
  : > "$component/Payload"
  cat > "$component/Scripts/postinstall" <<'POSTINSTALL'
#!/bin/sh
exec /opt/ScratchBird/libexec/scratchbird-macos-system-install post-install --package-version '0.0.0-nightly'
POSTINSTALL
  chmod 755 "$component/Scripts/postinstall"
fi
""",
        )
        write_tool(
            "ditto",
            """#!/usr/bin/env bash
set -euo pipefail
[[ "${1:-}" == "-x" ]] || exit 1
payload_root="${3:?missing payload root}"
runtime="$payload_root/opt/ScratchBird"
mkdir -p "$runtime/bin" "$runtime/lib" \\
  "$runtime/libexec" "$runtime/share/scratchbird/resources" \\
  "$runtime/share/scratchbird/config-defaults" \\
  "$runtime/share/scratchbird/release" \\
  "$payload_root/Library/LaunchDaemons"
for name in INSTALL_MANIFEST.json SHA256SUMS NATIVE_RELEASE_PROFILE.json \\
  MACOS_SUPPORT_MATRIX.json MACOS_SYSTEM_INSTALL_PROFILE.json \\
  MACOS_LAUNCHD_MANIFEST.json; do
  : > "$runtime/share/scratchbird/release/$name"
done
for binary in SBsql SBadm SBbak SBsec SBdoc SBcop SBsrv SBgate SBmgr SBParser; do
  cat > "$runtime/bin/$binary" <<'BINARY'
#!/usr/bin/env bash
printf 'fixture help\\n'
BINARY
  chmod 755 "$runtime/bin/$binary"
done
cat > "$runtime/libexec/scratchbird-macos-system-install" <<'HELPER'
#!/usr/bin/env bash
set -euo pipefail
action="${1:?missing lifecycle action}"
shift
root=""
while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --root)
      root="$2"
      shift 2
      ;;
    *)
      shift
      ;;
  esac
done
case "$action" in
  post-install)
    mkdir -p "$root/Library/Application Support/ScratchBird"
    ;;
  pre-remove)
    mkdir -p "$root/var/lib/scratchbird/install/config-preserve"
    cp "$root/Library/Application Support/ScratchBird/local-preserve.conf" \\
      "$root/var/lib/scratchbird/install/config-preserve/local-preserve.conf"
    ;;
esac
HELPER
chmod 755 "$runtime/libexec/scratchbird-macos-system-install"
: > "$payload_root/Library/LaunchDaemons/com.scratchbird.sbsrv.plist"
: > "$payload_root/Library/LaunchDaemons/com.scratchbird.sbmgr.plist"
""",
        )
        for name in ("python3", "otool", "plutil", "spctl"):
            write_tool(name, "#!/usr/bin/env bash\nexit 0\n")

        package = self.root / "expanded-scripts.pkg"
        package.write_bytes(b"fixture pkg\n")
        work_root = Path("expanded-scripts-smoke")
        environment = os.environ.copy()
        environment["PATH"] = f"{tools}{os.pathsep}{environment['PATH']}"
        result = subprocess.run(
            [str(smoke), str(package), str(work_root)],
            cwd=self.root,
            env=environment,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(
            result.returncode,
            0,
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )
        self.assertTrue((self.root / work_root / "pkg-scripts/postinstall").is_file())
        self.assertIn("smoke_install_macos=passed", result.stdout)

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
