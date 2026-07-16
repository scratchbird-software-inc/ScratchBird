#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import json
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import stage_native_release_bundle as stage


TOOLS_ROOT = Path(__file__).resolve().parent
VERIFY = TOOLS_ROOT / "verify_native_release_bundle.py"


class NativeReleaseBundleTest(unittest.TestCase):
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

    def fixture(self, root: Path) -> Path:
        source = root / "proof-output" / "output" / "linux"
        for rel in ("bin", "lib", "etc/scratchbird", "share/scratchbird/resources"):
            (source / rel).mkdir(parents=True, exist_ok=True)
        for name in stage.NATIVE_EXECUTABLES:
            (source / "bin" / name).write_bytes(b"native executable\n")
        for name in ("libSBcore.so", "libSBcore_static.a", "libSBParser_udr.a"):
            (source / "lib" / name).write_bytes(b"native library\n")
        for name in stage.NATIVE_CONFIGS:
            tokens = stage.REQUIRED_CONFIG_TOKENS[name]
            if name == "SBsrv.conf":
                section_tokens = {
                    "server.security": tokens[0:2],
                    "server.database": tokens[2:3],
                    "server.listener.native": tokens[3:6],
                    "server.memory": tokens[6:7],
                }
                config_text = "\n".join(
                    line
                    for section, section_values in section_tokens.items()
                    for line in (f"[{section}]", *section_values)
                )
            else:
                config_text = "\n".join(tokens)
            (source / "etc" / "scratchbird" / name).write_text(
                config_text + "\n", encoding="utf-8"
            )
        share_root = source / "share" / "scratchbird"
        for rel in stage.REQUIRED_RESOURCE_DIRS:
            (share_root / rel).mkdir(parents=True, exist_ok=True)
        for rel in stage.REQUIRED_OPERABILITY_FILES:
            path = share_root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture\n", encoding="utf-8")

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

    def verify(self, output: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(VERIFY), str(output), "--platform", "linux"],
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

    def test_verifier_rejects_commented_false_auto_create_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            output = root / "native" / "output" / "linux"
            stage.stage(source, output, "linux")
            server_config = output / "etc/scratchbird/SBsrv.conf"
            server_config.write_text(
                server_config.read_text(encoding="utf-8").replace(
                    "auto_create = false",
                    "# auto_create = false\nauto_create = true",
                    1,
                ),
                encoding="utf-8",
            )
            result = self.verify(output)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("native_config_active_token_cardinality", result.stdout)

    def test_verifier_rejects_commented_parser_path_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = self.fixture(root)
            output = root / "native" / "output" / "linux"
            stage.stage(source, output, "linux")
            parser_config = output / "etc/scratchbird/SBParser.conf"
            parser_config.write_text(
                parser_config.read_text(encoding="utf-8").replace(
                    "parser.worker_binary = bin/SBParser",
                    "# parser.worker_binary = bin/SBParser\n"
                    "parser.worker_binary = /tmp/untrusted-parser",
                    1,
                ),
                encoding="utf-8",
            )
            result = self.verify(output)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("native_config_active_token_cardinality", result.stdout)

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


if __name__ == "__main__":
    unittest.main()
