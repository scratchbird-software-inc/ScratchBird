#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest


def load_gate(path: pathlib.Path):
    sys.path.insert(0, str(path.parent))
    spec = importlib.util.spec_from_file_location(
        "parser_family_package_isolation_gate", path
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load gate: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class PackageIsolationGateTests(unittest.TestCase):
    gate = None

    def manifest(self) -> dict[str, object]:
        return {
            "parser_family_uuid": "parser.compatibility.mysql",
            "same_family_library_set": [
                {
                    "target": "sbp_mysql",
                    "artifact": "bin/sbp_mysql",
                    "owner": "parser.compatibility.mysql",
                },
                {
                    "target": "sbl_mysql_parser_pipeline",
                    "artifact": "lib/libsbl_mysql_parser_pipeline",
                    "owner": "parser.compatibility.mysql",
                },
                {
                    "target": "sbu_mysql_parser_support",
                    "artifact": "lib/libsbu_mysql_parser_support",
                    "owner": "parser.compatibility.mysql",
                },
            ],
            "neutral_dependency_set": [
                {
                    "target": "sbl_compatibility_parser_common",
                    "artifact": "lib/libsbl_compatibility_parser_common",
                    "owner": "family_neutral",
                },
                {
                    "target": "OpenSSL::Crypto",
                    "artifact": "system/libcrypto",
                    "owner": "system_neutral",
                },
            ],
        }

    def descriptor(self) -> dict[str, object]:
        return {
            "schema_version": 1,
            "parser_family": "mysql",
            "install_component": "parser_mysql_standalone",
            "same_family_targets": [
                "sbp_mysql",
                "sbl_mysql_parser_pipeline",
                "sbu_mysql_parser_support",
            ],
            "neutral_targets": ["sbl_compatibility_parser_common"],
        }

    def create_valid_prefix(self, root: pathlib.Path) -> None:
        files = (
            "bin/sbp_mysql",
            "lib/libsbl_mysql_parser_pipeline.a",
            "lib/libsbu_mysql_parser_support.a",
            "lib/libsbl_compatibility_parser_common.a",
            "share/scratchbird/parsers/mysql/standalone-package.json",
        )
        for relative in files:
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture", encoding="utf-8")
        (root / "share/scratchbird/parsers/mysql/standalone-package.json").write_text(
            json.dumps(self.descriptor()), encoding="utf-8"
        )

    def test_complete_declared_prefix_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            self.create_valid_prefix(root)
            gaps, declared = self.gate.validate_installed_closure(
                root, "mysql", self.manifest(), self.descriptor()
            )
        self.assertEqual([], gaps)
        self.assertIn("bin/sbp_mysql", declared)

    def test_undeclared_installed_artifact_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            self.create_valid_prefix(root)
            foreign = root / "lib" / "libsbl_postgresql_parser_pipeline.a"
            foreign.write_text("foreign", encoding="utf-8")
            gaps, _ = self.gate.validate_installed_closure(
                root, "mysql", self.manifest(), self.descriptor()
            )
        self.assertTrue(any("undeclared_installed_artifacts" in gap for gap in gaps), gaps)

    def test_missing_declared_artifact_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            self.create_valid_prefix(root)
            (root / "lib" / "libsbu_mysql_parser_support.a").unlink()
            gaps, _ = self.gate.validate_installed_closure(
                root, "mysql", self.manifest(), self.descriptor()
            )
        self.assertTrue(any("missing_declared_artifact" in gap for gap in gaps), gaps)

    def test_descriptor_target_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            self.create_valid_prefix(root)
            descriptor = self.descriptor()
            descriptor["neutral_targets"] = ["sbl_manager_protocol"]
            gaps, _ = self.gate.validate_installed_closure(
                root, "mysql", self.manifest(), descriptor
            )
        self.assertIn("package_descriptor.neutral_targets=manifest_exact", gaps)

    def test_operational_output_requires_owned_sblr_envelope(self) -> None:
        valid = json.dumps({
            "dialect": "mysql",
            "envelope": "SBLRExecutionEnvelope.v3",
            "operation_family": "query",
        })
        self.assertEqual([], self.gate.operational_output_gaps("mysql", valid))
        invalid = json.dumps({
            "dialect": "postgresql",
            "envelope": "SBLRExecutionEnvelope.v3",
            "operation_family": "query",
        })
        self.assertTrue(self.gate.operational_output_gaps("mysql", invalid))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gate-script", required=True, type=pathlib.Path)
    args = parser.parse_args()
    PackageIsolationGateTests.gate = load_gate(args.gate_script.resolve())
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(PackageIsolationGateTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
