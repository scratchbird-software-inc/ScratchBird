#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import contextlib
import csv
import io
import json
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

import stage_native_release_bundle as stage


TOOLS_ROOT = Path(__file__).resolve().parent
VERIFY = TOOLS_ROOT / "verify_native_release_bundle.py"
VERIFY_INSTALLED = TOOLS_ROOT / "verify_native_installed_payload.py"
INSTALLER_TOOLS_ROOT = TOOLS_ROOT.parent / "installers"
if str(INSTALLER_TOOLS_ROOT) not in sys.path:
    sys.path.insert(0, str(INSTALLER_TOOLS_ROOT))

import build_installers as installers


class NativeReleaseBundleTest(unittest.TestCase):
    def test_native_installer_admission_requires_verified_native_stage(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            output = root / "native" / "output" / "linux"
            stage.stage(source, output, "linux")

            digest = installers.require_staged_output(
                output,
                "linux",
                require_native_only=True,
            )
            self.assertEqual(
                digest,
                installers.sha256_file(output / "NATIVE_RELEASE_PROFILE.json"),
            )

            installer_output = root / "installer-output"
            installer_output.mkdir()
            (installer_output / "scratchbird-linux-test.tar.gz").write_bytes(
                b"installer fixture"
            )
            manifest_path = installers.write_artifact_manifest(
                installer_output,
                "linux",
                "0.0.0-test",
                "test-build",
                native_profile_digest=digest,
            )
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            admission = manifest.get("native_server_admission")
            self.assertIsInstance(admission, dict)
            self.assertEqual(admission["admission_controller"], "native_server_only")
            self.assertFalse(admission["client_artifacts_permitted"])
            self.assertEqual(admission["admitted_driver_adaptor_mcp_components"], [])
            self.assertTrue(admission["dbeaver_hard_excluded"])
            self.assertEqual(admission["native_release_profile_sha256"], digest)

            (output / "lib" / "libscratchbird_client.a").write_bytes(
                b"client leakage"
            )
            with self.assertRaises(SystemExit):
                installers.require_staged_output(
                    output,
                    "linux",
                    require_native_only=True,
                )

    def test_pe_dependency_parser_and_policy(self) -> None:
        output = """
          DLL Name: KERNEL32.dll
          DLL Name: libstdc++-6.dll
          DLL Name: libcrypto-3-x64.dll
        """
        self.assertEqual(
            stage.parse_pe_dependencies(output),
            {"KERNEL32.dll", "libstdc++-6.dll", "libcrypto-3-x64.dll"},
        )
        self.assertTrue(stage.windows_system_dll("KERNEL32.dll"))
        self.assertTrue(stage.safe_runtime_dependency_name("libstdc++-6.dll"))
        self.assertFalse(stage.safe_runtime_dependency_name("sbp_firebird.dll"))
        self.assertFalse(stage.safe_runtime_dependency_name("scratchbird_odbc.dll"))
        self.assertFalse(
            stage.safe_runtime_dependency_name("scratchbird_mojo_client_bridge.dll")
        )
        self.assertFalse(stage.safe_runtime_dependency_name("DBeaver-plugin.dll"))
        self.assertEqual(
            stage.admitted_windows_runtime_dependency(
                "libstdc++-6.dll",
                "libLLVM-22.dll",
            ),
            "libstdc++-6.dll",
        )

    def test_windows_explicit_llvm_runtime_is_copied_before_pe_recursion(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output = root / "output"
            search = root / "runtime"
            (output / "bin").mkdir(parents=True)
            search.mkdir()
            (output / "bin" / "SBsrv.exe").write_bytes(b"fixture")
            (search / "libLLVM-22.dll").write_bytes(b"llvm")
            (search / "libstdc++-6.dll").write_bytes(b"stdlib")

            def objdump_result(command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
                name = Path(command[-1]).name
                dependencies = {
                    "SBsrv.exe": "DLL Name: KERNEL32.dll\n",
                    "libLLVM-22.dll": "DLL Name: libstdc++-6.dll\n",
                    "libstdc++-6.dll": "DLL Name: KERNEL32.dll\n",
                }
                return subprocess.CompletedProcess(command, 0, dependencies[name], "")

            with mock.patch.object(stage.shutil, "which", return_value="objdump"), mock.patch.object(
                stage.subprocess, "run", side_effect=objdump_result
            ):
                copied = stage.stage_windows_runtime_dependencies(
                    output,
                    (search,),
                    ("libLLVM-22.dll",),
                )
            self.assertEqual(copied, ["libLLVM-22.dll", "libstdc++-6.dll"])
            self.assertTrue((output / "bin" / "libLLVM-22.dll").is_file())
            self.assertTrue((output / "bin" / "libstdc++-6.dll").is_file())

    def test_windows_client_dll_dependency_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output = root / "output"
            search = root / "runtime"
            (output / "bin").mkdir(parents=True)
            search.mkdir()
            (output / "bin" / "SBsrv.exe").write_bytes(b"fixture")
            (search / "scratchbird_odbc.dll").write_bytes(b"driver")

            def objdump_result(command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
                return subprocess.CompletedProcess(
                    command,
                    0,
                    "DLL Name: scratchbird_odbc.dll\n",
                    "",
                )

            with mock.patch.object(stage.shutil, "which", return_value="objdump"), mock.patch.object(
                stage.subprocess, "run", side_effect=objdump_result
            ):
                with self.assertRaises(SystemExit):
                    stage.stage_windows_runtime_dependencies(output, (search,))

    def test_windows_unknown_neutral_dll_dependency_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output = root / "output"
            search = root / "runtime"
            (output / "bin").mkdir(parents=True)
            search.mkdir()
            (output / "bin" / "SBsrv.exe").write_bytes(b"fixture")
            (search / "neutral-client.dll").write_bytes(b"driver")

            def objdump_result(command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
                return subprocess.CompletedProcess(
                    command,
                    0,
                    "DLL Name: neutral-client.dll\n",
                    "",
                )

            with mock.patch.object(stage.shutil, "which", return_value="objdump"), mock.patch.object(
                stage.subprocess, "run", side_effect=objdump_result
            ):
                with self.assertRaises(SystemExit):
                    stage.stage_windows_runtime_dependencies(output, (search,))

    def test_windows_runtime_inventory_requires_the_reviewed_closure(self) -> None:
        stage.require_windows_runtime_inventory(set(stage.WINDOWS_NATIVE_RUNTIME_NAMES))
        with self.assertRaises(SystemExit):
            stage.require_windows_runtime_inventory({"libLLVM-22.dll"})

    def fixture(self, root: Path) -> Path:
        source = root / "proof-output" / "output" / "linux"
        for rel in ("bin", "lib", "etc/scratchbird", "share/scratchbird/resources"):
            (source / rel).mkdir(parents=True, exist_ok=True)
        for name in stage.NATIVE_EXECUTABLES:
            (source / "bin" / name).write_bytes(b"native executable\n")
        for name in ("libSBcore.so", "libSBcore_static.a", "libSBParser_udr.a"):
            (source / "lib" / name).write_bytes(b"native library\n")
        for name in stage.NATIVE_CONFIGS:
            (source / "etc" / "scratchbird" / name).write_text(
                self.config_fixture_text(name), encoding="utf-8"
            )
        share_root = source / "share" / "scratchbird"
        for rel in stage.REQUIRED_RESOURCE_DIRS:
            (share_root / rel).mkdir(parents=True, exist_ok=True)
        for rel, source_rel in stage.NATIVE_SHARE_NONRESOURCE_SOURCE_FILES.items():
            path = share_root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(stage.PUBLIC_REPO_ROOT / source_rel, path)

        seed_root = share_root / "resources/seed-packs/initial-resource-pack"
        resource_files = (
            "resources/charsets/charsets.json",
            "resources/collations/collations.json",
            "resources/timezones/version",
            "resources/i18n/version",
            "resources/i18n/sbsql-language-resource-pack/manifest.sblrp.json",
            "resources/i18n/sbsql-language-resource-pack/manifest.sblrp.sig",
            "resources/i18n/sbsql-language-resource-pack/hashes.sha256",
        )
        for rel in resource_files:
            path = seed_root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("{}\n" if path.suffix == ".json" else "fixture\n", encoding="utf-8")
        manifest_rows = [
            "seed_family,source_pattern,required_catalog_rows,create_time_action,status",
            "charset,resources/charsets/charsets.json,fixture,fixture,specified",
            "collation,resources/collations/collations.json,fixture,fixture,specified",
            "timezone,resources/timezones/version,fixture,fixture,specified",
            "i18n,resources/i18n/version,fixture,fixture,specified",
            "sbsql_language_resource_pack,resources/i18n/sbsql-language-resource-pack/manifest.sblrp.json,fixture,fixture,specified",
        ]
        (seed_root / "RESOURCE_SEED_MANIFEST.csv").write_text(
            "\n".join(manifest_rows) + "\n", encoding="utf-8"
        )
        artifact_rows = ["canonical_path,content_hash,content_size_bytes"]
        for rel in resource_files:
            content = stage.canonical_resource_bytes(seed_root / rel)
            artifact_rows.append(f"{rel},{stage.fnv1a64(content)},{len(content)}")
        (seed_root / "RESOURCE_SEED_ARTIFACTS.csv").write_text(
            "\n".join(artifact_rows) + "\n", encoding="utf-8"
        )

        policy_root = share_root / "resources/policy-packs/default-local-password"
        policy_files = (
            "catalog_materialization.json",
            "policies/security_providers.json",
            "policies/roles.json",
            "policies/groups.json",
            "policies/grants.json",
            "policies/policy_profiles.json",
            "policies/server_memory_cache_policy.json",
            "policies/default_policy_catalog.json",
            "policies/policy_defaults.json",
        )
        content_manifest = []
        aggregate = bytearray()
        for rel in policy_files:
            path = policy_root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("{}\n", encoding="utf-8")
            digest = hashlib.sha256(stage.canonical_policy_bytes(path)).hexdigest()
            content_manifest.append({"path": rel, "sha256": digest})
            aggregate.extend(rel.encode("utf-8") + b"\0" + digest.encode("ascii") + b"\n")
        (policy_root / "POLICY_PACK_MANIFEST.json").write_text(
            json.dumps(
                {
                    "policy_pack_id": "default-local-password",
                    "content_manifest": content_manifest,
                    "content_sha256": hashlib.sha256(aggregate).hexdigest(),
                }
            )
            + "\n",
            encoding="utf-8",
        )
        (source / "STANDALONE_OUTPUT_MANIFEST.json").write_text(
            json.dumps(
                {
                    "platform": "linux",
                    "runtime_requirements": {
                        "llvm": {
                            "link_mode": "dynamic",
                            "runtime_library": "libLLVM.so.23.0",
                            "delivery": "system-package",
                            "minimum_major": 23,
                        }
                    },
                }
            )
            + "\n"
        )
        (source / "bin" / "sbp_firebird").write_bytes(b"must not ship\n")
        (source / "lib" / "libsbl_firebird_parser_pipeline.a").write_bytes(
            b"must not ship\n"
        )
        (source / "bin" / "some_test_probe").write_bytes(b"must not ship\n")
        unrelated_example = (
            share_root / "examples" / "example_database" / "credentials.txt"
        )
        unrelated_example.parent.mkdir(parents=True)
        unrelated_example.write_text(
            "unrelated example credential fixture must not ship\n", encoding="utf-8"
        )
        return source

    @staticmethod
    def write_timezone_archive(
        path: Path,
        members: tuple[tuple[str, bytes, int], ...],
    ) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(path, mode="w:gz") as archive:
            for name, content, mode in members:
                member = tarfile.TarInfo(name)
                member.size = len(content)
                member.mode = mode
                archive.addfile(member, io.BytesIO(content))

    @staticmethod
    def declare_seed_artifact(seed_root: Path, relative: str) -> None:
        artifact_index = seed_root / "RESOURCE_SEED_ARTIFACTS.csv"
        with artifact_index.open(newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            fieldnames = reader.fieldnames
            rows = list(reader)
        if fieldnames != [
            "canonical_path",
            "content_hash",
            "content_size_bytes",
        ]:
            raise AssertionError(f"unexpected seed artifact header: {fieldnames}")
        rows = [row for row in rows if row["canonical_path"] != relative]
        content = stage.canonical_resource_bytes(seed_root / relative)
        rows.append(
            {
                "canonical_path": relative,
                "content_hash": stage.fnv1a64(content),
                "content_size_bytes": str(len(content)),
            }
        )
        with artifact_index.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(rows)

    @staticmethod
    def config_fixture_text(name: str) -> str:
        if name != "SBsrv.conf":
            return "\n".join(stage.REQUIRED_CONFIG_TOKENS[name]) + "\n"
        return (
            "[server.security]\n"
            "provider_family = local_password\n"
            "default_policy_installed = true\n"
            "[server.database]\n"
            "auto_create = false\n"
            "[server.listener]\n"
            "executable_path = bin/SBgate\n"
            "control_dir = runtime/listener/control\n"
            "runtime_dir = runtime/listener/runtime\n"
            "[server.parser]\n"
            "sbps_enabled = true\n"
            "sbps_endpoint = runtime/control/sb_server.sbps.sock\n"
            "[server.memory]\n"
            "failure_mode = return_error\n"
        )

    def verify(self, output: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(VERIFY), str(output), "--platform", "linux"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )

    def installed_payload(self, output: Path, payload: Path) -> Path:
        runtime_root = payload / "opt" / "ScratchBird"
        for name in ("bin", "lib", "share"):
            shutil.copytree(output / name, runtime_root / name)
        shutil.copytree(output / "etc", payload / "etc")
        release_root = runtime_root / "share" / "scratchbird" / "release"
        release_root.mkdir(parents=True, exist_ok=True)
        for name in (
            "STANDALONE_OUTPUT_MANIFEST.json",
            "NATIVE_RELEASE_PROFILE.json",
        ):
            shutil.copy2(output / name, release_root / name)
        return runtime_root

    def verify_installed(
        self,
        payload: Path,
        config_root: Path | None = None,
    ) -> subprocess.CompletedProcess[str]:
        command = [sys.executable, str(VERIFY_INSTALLED), str(payload)]
        if config_root is not None:
            command.extend(("--config-root", str(config_root)))
        return subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )

    def test_stage_excludes_emulation_and_test_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            output = root / "native" / "output" / "linux"
            stage.stage(source, output, "linux")
            self.assertFalse((output / "bin" / "sbp_firebird").exists())
            self.assertFalse((output / "lib" / "libsbl_firebird_parser_pipeline.a").exists())
            self.assertFalse((output / "bin" / "some_test_probe").exists())
            self.assertFalse(
                (output / "share/scratchbird/examples/example_database").exists()
            )
            result = self.verify(output)
            self.assertEqual(result.returncode, 0, result.stdout)

    def test_stage_excludes_client_driver_adaptor_and_mcp_libraries(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            client_libraries = (
                "libscratchbird_client.a",
                "libscratchbird_odbc.so",
                "libscratchbird_mojo_client_bridge.so",
                "libscratchbird_python.so",
            )
            for name in client_libraries:
                (source / "lib" / name).write_bytes(b"must not ship\n")

            output = root / "native" / "output" / "linux"
            stage.stage(source, output, "linux")
            for name in client_libraries:
                self.assertFalse((output / "lib" / name).exists(), name)
            result = self.verify(output)
            self.assertEqual(result.returncode, 0, result.stdout)

            injected = output / "lib" / "libscratchbird_odbc.so"
            injected.write_bytes(b"driver leakage\n")
            result = self.verify(output)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("non_native_library_forbidden", result.stdout)

    def test_stage_rejects_client_subtree_hidden_in_shared_resources(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            hidden = source / "share/scratchbird/resources/adaptor/opaque-client.dat"
            hidden.parent.mkdir(parents=True)
            hidden.write_bytes(b"not a native resource\n")

            with self.assertRaises(SystemExit):
                stage.stage(source, root / "native" / "output" / "linux", "linux")

    def test_native_share_identity_policy_hard_rejects_all_client_families(self) -> None:
        for relative in (
            "resources/drivers/python/opaque.bin",
            "resources/adaptor/neutral/opaque.bin",
            "resources/adapter/neutral/opaque.bin",
            "resources/mcp/neutral/opaque.bin",
            "docs/public_api/DBeaver-plugin.txt",
            "examples/native_release_qa/scratchbird-mojo-client-bridge.txt",
        ):
            self.assertTrue(
                stage.native_share_path_has_client_identity(Path(relative)),
                relative,
            )
        self.assertFalse(
            stage.native_share_path_has_client_identity(
                Path("examples/core_beta_qa/driver_route_smoke.sh")
            )
        )

    def test_stage_rejects_uninventoried_neutral_docs_and_example_sources(self) -> None:
        for relative in (
            "examples/native_release_qa/opaque.py",
            "docs/release/opaque-source.md",
        ):
            with self.subTest(relative=relative), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                source = self.fixture(root)
                injected = source / "share" / "scratchbird" / relative
                injected.parent.mkdir(parents=True, exist_ok=True)
                injected.write_text("unfinished component source\n", encoding="utf-8")

                with self.assertRaises(SystemExit):
                    stage.stage(source, root / "native" / "output" / "linux", "linux")

    def test_stage_rejects_modified_driver_route_script(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            route = (
                source
                / "share"
                / "scratchbird"
                / "examples"
                / "core_beta_qa"
                / "driver_route_smoke.sh"
            )
            route.write_text(
                "#!/usr/bin/env bash\nexec ./unfinished-client-component\n",
                encoding="utf-8",
            )

            with self.assertRaises(SystemExit):
                stage.stage(source, root / "native" / "output" / "linux", "linux")

    def test_declared_timezone_archives_are_recursively_payload_checked(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            seed_root = (
                source
                / "share/scratchbird/resources/seed-packs/initial-resource-pack"
            )
            relative = "resources/timezones/tzdata2025c.tar.gz"
            archive = seed_root / relative

            self.write_timezone_archive(
                archive,
                (("version", b"2025c\n", 0o644),),
            )
            self.declare_seed_artifact(seed_root, relative)
            output = root / "native" / "output" / "linux"
            stage.stage(source, output, "linux")
            result = self.verify(output)
            self.assertEqual(result.returncode, 0, result.stdout)

            payload = root / "payload"
            runtime_root = self.installed_payload(output, payload)
            installed_seed_root = (
                runtime_root
                / "share/scratchbird/resources/seed-packs/initial-resource-pack"
            )
            self.write_timezone_archive(
                installed_seed_root / relative,
                (("neutral-data.bin", b"\x7fELFhidden-client-payload", 0o644),),
            )
            self.declare_seed_artifact(installed_seed_root, relative)
            result = self.verify_installed(payload)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("native_share_archive_member_payload_forbidden", result.stdout)

            cases = (
                (
                    "../outside.txt",
                    b"unsafe member path\n",
                    "native_share_archive_member_path_unsafe",
                ),
                (
                    "drivers/opaque.dat",
                    b"client content\n",
                    "native_share_archive_member_client_forbidden",
                ),
                (
                    "neutral-data.bin",
                    b"\x7fELFhidden-client-payload",
                    "native_share_archive_member_payload_forbidden",
                ),
            )
            for member_name, content, marker in cases:
                self.write_timezone_archive(archive, ((member_name, content, 0o644),))
                self.declare_seed_artifact(seed_root, relative)
                errors = io.StringIO()
                with contextlib.redirect_stderr(errors), self.assertRaises(SystemExit):
                    stage.stage(source, output, "linux")
                self.assertIn(marker, errors.getvalue())

            opaque_relative = "resources/timezones/opaque.tar.gz"
            self.write_timezone_archive(
                seed_root / opaque_relative,
                (("version", b"opaque archive\n", 0o644),),
            )
            self.declare_seed_artifact(seed_root, opaque_relative)
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors), self.assertRaises(SystemExit):
                stage.stage(source, output, "linux")
            self.assertIn("native_share_archive_location_forbidden", errors.getvalue())

    def test_staged_and_installed_verifiers_reject_renamed_share_binary(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            output = root / "native" / "output" / "linux"
            stage.stage(source, output, "linux")
            hidden = output / "share/scratchbird/resources/diagnostics/neutral-data.bin"
            hidden.parent.mkdir(parents=True)
            hidden.write_bytes(b"\x7fELFhidden-client-payload")

            result = self.verify(output)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("native_share_executable_payload_forbidden", result.stdout)

            payload = root / "payload"
            self.installed_payload(output, payload)
            result = self.verify_installed(payload)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("native_share_executable_payload_forbidden", result.stdout)

    def test_stage_rejects_default_server_listener_profile(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            server_config = source / "etc/scratchbird/SBsrv.conf"
            server_config.write_text(
                server_config.read_text(encoding="utf-8")
                + "[server.listener.profile.sbsql]\n"
                + "enabled = true\n",
                encoding="utf-8",
            )
            with self.assertRaises(SystemExit):
                stage.stage(source, root / "native" / "output" / "linux", "linux")

    def test_stage_rejects_duplicate_server_listener_executable(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            server_config = source / "etc/scratchbird/SBsrv.conf"
            server_config.write_text(
                server_config.read_text(encoding="utf-8")
                + "[server.listener]\n"
                + "executable_path = bin/SBParser\n",
                encoding="utf-8",
            )
            with self.assertRaises(SystemExit):
                stage.stage(source, root / "native" / "output" / "linux", "linux")

    def test_installed_config_validator_accepts_materialized_linux_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            config_root = source / "etc/scratchbird"
            replacements = {
                "SBsrv.conf": {
                    "executable_path = bin/SBgate": (
                        "executable_path = /opt/ScratchBird/bin/SBgate"
                    ),
                },
                "SBgate.conf": {
                    "parser_executable = bin/SBParser": (
                        "parser_executable = /opt/ScratchBird/bin/SBParser"
                    ),
                },
                "SBParser.conf": {
                    "parser.worker_binary = bin/SBParser": (
                        "parser.worker_binary = /opt/ScratchBird/bin/SBParser"
                    ),
                },
                "SBbootstrap.profile": {
                    "platform = operator_required": "platform = linux",
                    "service_identity = operator_required": (
                        "service_identity = scratchbird"
                    ),
                    "service_group = operator_required": "service_group = scratchbird",
                },
            }
            for name, mapping in replacements.items():
                path = config_root / name
                text = path.read_text(encoding="utf-8")
                for old, new in mapping.items():
                    self.assertEqual(text.count(old), 1)
                    text = text.replace(old, new)
                path.write_text(text, encoding="utf-8")
            stage.require_native_installed_configs(config_root, source, "linux")

    def test_installed_config_validator_accepts_windows_placeholder_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            for binary in ("SBgate", "SBParser"):
                shutil.copy2(
                    source / "bin" / binary,
                    source / "bin" / f"{binary}.exe",
                )
            config_root = source / "etc/scratchbird"
            replacements = {
                "SBsrv.conf": {
                    "executable_path = bin/SBgate": (
                        "executable_path = @SCRATCHBIRD_INSTALL_ROOT@/bin/SBgate.exe"
                    ),
                },
                "SBgate.conf": {
                    "parser_executable = bin/SBParser": (
                        "parser_executable = @SCRATCHBIRD_INSTALL_ROOT@/bin/SBParser.exe"
                    ),
                },
                "SBParser.conf": {
                    "parser.worker_binary = bin/SBParser": (
                        "parser.worker_binary = @SCRATCHBIRD_INSTALL_ROOT@/bin/SBParser.exe"
                    ),
                },
                "SBbootstrap.profile": {
                    "platform = operator_required": "platform = windows",
                    "service_identity = operator_required": (
                        r"service_identity = NT SERVICE\scratchbird"
                    ),
                    "service_group = operator_required": (
                        "service_group = ScratchBird"
                    ),
                },
            }
            for name, mapping in replacements.items():
                path = config_root / name
                text = path.read_text(encoding="utf-8")
                for old, new in mapping.items():
                    self.assertEqual(text.count(old), 1)
                    text = text.replace(old, new)
                path.write_text(text, encoding="utf-8")
            stage.require_native_installed_configs(config_root, source, "windows")

    def test_installed_payload_verifier_accepts_materialized_linux_config(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            output = root / "native" / "output" / "linux"
            stage.stage(source, output, "linux")
            payload = root / "payload"
            self.installed_payload(output, payload)
            config_root = payload / "etc" / "scratchbird"
            replacements = {
                "SBsrv.conf": {
                    "executable_path = bin/SBgate": (
                        "executable_path = /opt/ScratchBird/bin/SBgate"
                    ),
                },
                "SBgate.conf": {
                    "parser_executable = bin/SBParser": (
                        "parser_executable = /opt/ScratchBird/bin/SBParser"
                    ),
                },
                "SBParser.conf": {
                    "parser.worker_binary = bin/SBParser": (
                        "parser.worker_binary = /opt/ScratchBird/bin/SBParser"
                    ),
                },
                "SBbootstrap.profile": {
                    "platform = operator_required": "platform = linux",
                    "service_identity = operator_required": (
                        "service_identity = scratchbird"
                    ),
                    "service_group = operator_required": "service_group = scratchbird",
                },
            }
            for name, mapping in replacements.items():
                path = config_root / name
                text = path.read_text(encoding="utf-8")
                for old, new in mapping.items():
                    self.assertEqual(text.count(old), 1)
                    text = text.replace(old, new)
                path.write_text(text, encoding="utf-8")
            result = self.verify_installed(payload, config_root)
            self.assertEqual(result.returncode, 0, result.stdout)

    def test_installed_payload_verifier_rejects_client_library_leakage(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            output = root / "native" / "output" / "linux"
            stage.stage(source, output, "linux")
            payload = root / "payload"
            runtime_root = self.installed_payload(output, payload)
            (runtime_root / "lib" / "libscratchbird_mojo_client_bridge.so").write_bytes(
                b"must not install\n"
            )

            result = self.verify_installed(payload)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("installed_library_set_mismatch", result.stdout)

    def test_installed_config_validator_rejects_untrusted_parser_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            config_root = source / "etc/scratchbird"
            listener_config = config_root / "SBgate.conf"
            listener_config.write_text(
                listener_config.read_text(encoding="utf-8").replace(
                    "parser_executable = bin/SBParser",
                    "parser_executable = /untrusted/bin/SBParser",
                ),
                encoding="utf-8",
            )
            with self.assertRaises(SystemExit):
                stage.require_native_installed_configs(config_root, source, "linux")

    def test_verifier_rejects_added_emulation_binary(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            output = root / "native" / "output" / "linux"
            stage.stage(source, output, "linux")
            (output / "bin" / "sbp_firebird").write_bytes(b"unexpected\n")
            result = self.verify(output)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("non_native_executable_forbidden", result.stdout)

    def test_verifier_rejects_parser_topology_relaxation(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            output = root / "native" / "output" / "linux"
            stage.stage(source, output, "linux")
            profile_path = output / "NATIVE_RELEASE_PROFILE.json"
            profile = json.loads(profile_path.read_text(encoding="utf-8"))
            profile["topology"]["direct_engine_link"] = "allowed"
            profile_path.write_text(
                json.dumps(profile, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            result = self.verify(output)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("native_profile_topology_contract_mismatch", result.stdout)

    def test_stage_fails_when_native_component_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            (source / "bin" / "SBsrv").unlink()
            with self.assertRaises(SystemExit):
                stage.stage(source, root / "native" / "output" / "linux", "linux")

    def test_stage_fails_when_llvm_runtime_contract_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            (source / "STANDALONE_OUTPUT_MANIFEST.json").write_text(
                json.dumps({"platform": "linux"}) + "\n", encoding="utf-8"
            )
            with self.assertRaises(SystemExit):
                stage.stage(source, root / "native" / "output" / "linux", "linux")

    def test_linux_llvm_runtime_contract_rejects_build_path(self) -> None:
        with self.assertRaises(SystemExit):
            stage.require_llvm_runtime_contract(
                {
                    "link_mode": "dynamic",
                    "runtime_library": "/build/libLLVM.so.23.0",
                    "delivery": "system-package",
                    "minimum_major": 23,
                },
                "linux",
            )

    def test_stage_fails_when_sbsql_language_pack_contract_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            language_manifest = (
                source
                / "share/scratchbird/resources/seed-packs/initial-resource-pack"
                / "resources/i18n/sbsql-language-resource-pack/manifest.sblrp.json"
            )
            language_manifest.unlink()
            with self.assertRaises(SystemExit):
                stage.stage(source, root / "native" / "output" / "linux", "linux")

    def test_verifier_rejects_tampered_resource_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            output = root / "native" / "output" / "linux"
            stage.stage(source, output, "linux")
            charset_catalog = (
                output
                / "share/scratchbird/resources/seed-packs/initial-resource-pack"
                / "resources/charsets/charsets.json"
            )
            charset_catalog.write_text("tampered\n", encoding="utf-8")
            result = self.verify(output)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("resource_artifact_hash_mismatch", result.stdout)

    def test_verifier_rejects_firebird_port_in_native_config(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            output = root / "native" / "output" / "linux"
            stage.stage(source, output, "linux")
            listener_config = output / "etc/scratchbird/SBgate.conf"
            listener_config.write_text(
                listener_config.read_text(encoding="utf-8").replace(
                    "port = 3092", "port = 3050"
                ),
                encoding="utf-8",
            )
            result = self.verify(output)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("native_config_forbidden_native_port", result.stdout)

    def test_verifier_rejects_manager_backend_front_door_alias(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            output = root / "native" / "output" / "linux"
            stage.stage(source, output, "linux")
            manager_config = output / "etc/scratchbird/SBmgr.conf"
            manager_config.write_text(
                manager_config.read_text(encoding="utf-8").replace(
                    "manager.backend.native_port = 0",
                    "manager.backend.native_port = 3092",
                ),
                encoding="utf-8",
            )
            result = self.verify(output)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("native_manager_backend_must_be_unset", result.stdout)

    def test_verifier_rejects_unexpected_example_subtree(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            output = root / "native" / "output" / "linux"
            stage.stage(source, output, "linux")
            unexpected = output / "share/scratchbird/examples/example_database"
            unexpected.mkdir(parents=True)
            (unexpected / "credentials.txt").write_text(
                "unrelated example credential fixture\n", encoding="utf-8"
            )
            result = self.verify(output)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("native_share_subtree_set_mismatch", result.stdout)

    def test_verifier_rejects_unexpected_runtime_root_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            output = root / "native" / "output" / "linux"
            stage.stage(source, output, "linux")
            injected = output / "extensions" / "opaque-client-component.bin"
            injected.parent.mkdir(parents=True)
            injected.write_bytes(b"\x7fELFincomplete client payload\n")

            result = self.verify(output)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("native_root_directory_set_mismatch", result.stdout)

    def test_installed_verifier_rejects_unexpected_runtime_root_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            output = root / "native" / "output" / "linux"
            stage.stage(source, output, "linux")
            payload = root / "payload"
            runtime_root = self.installed_payload(output, payload)
            injected = runtime_root / "extensions" / "opaque-client-component.bin"
            injected.parent.mkdir(parents=True)
            injected.write_bytes(b"\x7fELFincomplete client payload\n")

            result = self.verify_installed(payload)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("installed_runtime_root_directory_set_mismatch", result.stdout)

    def test_installed_verifier_rejects_payload_sibling_client_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            output = root / "native" / "output" / "linux"
            stage.stage(source, output, "linux")
            payload = root / "payload"
            self.installed_payload(output, payload)
            injected = payload / "driver" / "opaque-client-component.bin"
            injected.parent.mkdir(parents=True)
            injected.write_bytes(b"\x7fELFincomplete client payload\n")

            result = self.verify_installed(payload)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("installed_payload_root_directory_set_mismatch", result.stdout)


if __name__ == "__main__":
    unittest.main()
