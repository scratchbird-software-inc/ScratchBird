#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest


def load_gate(path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("parser_family_binary_isolation_gate", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load gate: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class BinaryIsolationGateTests(unittest.TestCase):
    gate = None
    ownership_map = pathlib.Path()

    @classmethod
    def registry(cls):
        return cls.gate.Registry(json.loads(cls.ownership_map.read_text(encoding="utf-8")))

    def test_foreign_link_package_symbol_and_runtime_fixtures_are_rejected(self) -> None:
        result = self.gate.negative_fixture_result(self.registry())
        self.assertTrue(result["passed"], result)
        self.assertEqual(result["fixture_count"], result["detected_count"])

    def test_neutral_system_dependencies_are_accepted(self) -> None:
        registry = self.registry()
        family = sorted(registry.families)[0]
        text = "libc.so.6 libstdc++.so.6 libpthread.so.0 /usr/lib/ld-linux-x86-64.so.2"
        self.assertEqual([], self.gate.scan_text(registry, family, "neutral", text))

    def test_strict_firebird_rejects_legacy_shared_parser_closure(self) -> None:
        registry = self.registry()
        fixtures = (
            "sbl_compatibility_parser_common",
            "lib/libsbl_compatibility_parser_common.a",
            "sbl_parser_neutral_evidence",
            "lib/libsbl_parser_neutral_evidence.a",
            "scratchbird::parser::compatibility::ParseStatement(std::string_view)",
            "scratchbird::parser::compatibility::HandleWorkerCommand(std::string_view)",
            "project/src/parsers/compatibility/common/compatibility_dialect.cpp",
        )
        for fixture in fixtures:
            with self.subTest(fixture=fixture):
                findings = self.gate.scan_text(
                    registry, "firebird", "strict_fixture", fixture
                )
                self.assertTrue(
                    any(row.foreign_family == "legacy_shared_parser"
                        for row in findings),
                    findings,
                )

    def test_family_name_prefix_does_not_create_false_foreign_owner(self) -> None:
        registry = self.registry()
        family = "opensearch_sql_ppl"
        text = " ".join((
            "project/src/parsers/compatibility/opensearch_sql_ppl/parser.cpp",
            "libsbl_opensearch_sql_ppl_parser_pipeline.a",
            "scratchbird::parser::opensearch_sql_ppl::ParseStatement()",
            "/run/scratchbird/parser_opensearch_sql_ppl.sock",
        ))
        self.assertEqual([], self.gate.scan_text(registry, family, "owned", text))

    def test_ninja_link_evidence_selects_exact_target(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            (root / "build.ninja").write_text(
                "build bin/sbp_opensearch_sql_ppl: "
                "CXX_EXECUTABLE_LINKER__sbp_opensearch_sql_ppl_Release "
                "main.cpp.o | libsbl_opensearch_sql_ppl_parser_pipeline.a\n"
                "  LINK_LIBRARIES = libsbl_opensearch_sql_ppl_parser_pipeline.a\n\n"
                "build bin/sbp_opensearch: "
                "CXX_EXECUTABLE_LINKER__sbp_opensearch_Release "
                "main.cpp.o | libsbl_opensearch_parser_pipeline.a\n"
                "  LINK_LIBRARIES = libsbl_opensearch_parser_pipeline.a\n\n",
                encoding="utf-8",
            )
            _, evidence = self.gate.link_command(root, "opensearch")
        self.assertIn("libsbl_opensearch_parser_pipeline.a", evidence)
        self.assertNotIn("opensearch_sql_ppl", evidence)

    def test_artifact_discovery_ignores_prior_evidence_stages(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            output_binary = root / "output" / "linux" / "bin" / "sbp_mysql"
            output_archive = (
                root / "output" / "linux" / "lib" / "libsbl_mysql_parser_pipeline.a"
            )
            staged_binary = root / "old-evidence" / "stage" / "mysql" / "bin" / "sbp_mysql"
            for path in (output_binary, output_archive, staged_binary):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"fixture")
            output_binary.chmod(0o755)
            staged_binary.chmod(0o755)
            artifacts = self.gate.discover_artifacts(root, self.registry())
        self.assertEqual(
            (output_binary.resolve(), output_archive.resolve()), artifacts["mysql"]
        )

    def valid_manifest(self, family: str = "mysql") -> dict[str, object]:
        family_uuid = self.gate.expected_family_uuid(family)
        return {
            "dialect": family,
            "parser_family_uuid": family_uuid,
            "standalone_package": True,
            "cross_parser_dependency_count": 0,
            "same_family_library_set": [
                {
                    "target": target,
                    "artifact": f"artifact/{target}",
                    "owner": family_uuid,
                }
                for target in sorted(self.gate.expected_same_family_targets(family))
            ],
            "neutral_dependency_set": [{
                "target": "sbl_compatibility_parser_common",
                "artifact": "lib/libsbl_compatibility_parser_common",
                "owner": "family_neutral",
                "version": "same-build",
            }],
            "parser_support_udr_family_uuid": family_uuid,
            "direct_sblr_lowering": True,
            "foreign_parser_fallback": False,
            "isolated_build_profile": "parser-family-isolated-release-v1",
            "isolated_package_profile": "parser-family-empty-prefix-v1",
            "dependency_closure_evidence": copy.deepcopy(
                self.gate.DEPENDENCY_EVIDENCE_REFERENCES
            ),
        }

    def test_canonical_manifest_is_accepted(self) -> None:
        manifest = self.valid_manifest()
        _, gaps = self.gate.parse_package_identity("mysql", json.dumps(manifest))
        self.assertEqual([], gaps)

    def test_contradictory_manifest_values_are_rejected(self) -> None:
        cases = {
            "wrong_owner": ("parser_family_uuid", "parser.compatibility.postgresql"),
            "not_standalone": ("standalone_package", False),
            "cross_parser": ("cross_parser_dependency_count", 1),
            "fallback": ("foreign_parser_fallback", True),
            "udr_mismatch": ("parser_support_udr_family_uuid", "parser.compatibility.postgresql"),
            "wrong_build_profile": ("isolated_build_profile", "default-release"),
            "wrong_package_profile": ("isolated_package_profile", "system-prefix"),
            "missing_evidence": ("dependency_closure_evidence", {"source": "evidence#source"}),
        }
        for name, (field, value) in cases.items():
            with self.subTest(name=name):
                manifest = copy.deepcopy(self.valid_manifest())
                manifest[field] = value
                _, gaps = self.gate.parse_package_identity("mysql", json.dumps(manifest))
                self.assertTrue(gaps, name)

    def test_foreign_owned_manifest_artifact_is_rejected(self) -> None:
        manifest = self.valid_manifest()
        manifest["same_family_library_set"].append({
            "target": "sbl_postgresql_parser_pipeline",
            "artifact": "lib/libsbl_postgresql_parser_pipeline",
            "owner": "parser.compatibility.postgresql",
        })
        _, gaps = self.gate.parse_package_identity("mysql", json.dumps(manifest))
        self.assertTrue(any("owner" in gap for gap in gaps), gaps)
        findings = self.gate.scan_text(
            self.registry(), "mysql", "package_manifest", json.dumps(manifest)
        )
        self.assertTrue(any(row.foreign_family == "postgresql" for row in findings))

    def test_link_closure_must_be_declared_by_manifest(self) -> None:
        manifest = self.valid_manifest()
        complete_link = (
            "lib/libsbl_mysql_parser_pipeline.a "
            "lib/libsbl_compatibility_parser_common.a"
        )
        self.assertEqual(
            [], self.gate.manifest_link_dependency_gaps(manifest, complete_link)
        )
        incomplete_link = complete_link + " lib/libsbl_manager_protocol.a"
        gaps = self.gate.manifest_link_dependency_gaps(manifest, incomplete_link)
        self.assertEqual(
            ["dependency_sets=complete_link_closure:sbl_manager_protocol"], gaps
        )

    def test_branded_sbsql_link_artifacts_map_to_declared_targets(self) -> None:
        manifest = self.valid_manifest("sbsql")
        link = "lib/libSBParser_pipeline.a lib/libSBParser_core.a"
        self.assertEqual([], self.gate.manifest_link_dependency_gaps(manifest, link))

    def test_foreign_branded_sbsql_artifacts_are_rejected(self) -> None:
        registry = self.registry()
        fixtures = (
            "lib/libSBParser_pipeline.a",
            "lib/libSBParser_core.so",
            "lib/libSBParser_udr.dylib",
            'execve("/stage/bin/SBParser", ["SBParser"], 0)',
            "bin/SBParser",
            "SBParser",
        )
        for fixture in fixtures:
            with self.subTest(fixture=fixture):
                findings = self.gate.scan_text(registry, "mysql", "fixture", fixture)
                self.assertTrue(
                    any(row.foreign_family == "sbsql" for row in findings), findings
                )

    def test_branded_sbsql_words_in_prose_do_not_create_false_owner(self) -> None:
        registry = self.registry()
        text = "The SBParser executable is described here but no artifact path is present."
        self.assertEqual([], self.gate.scan_text(registry, "mysql", "prose", text))

    def test_duplicate_manifest_keys_are_rejected(self) -> None:
        output = json.dumps(self.valid_manifest())
        output = output[:-1] + ', "standalone_package": false}'
        payload, gaps = self.gate.parse_package_identity("mysql", output)
        self.assertIsNone(payload)
        self.assertIn("duplicate_json_key:standalone_package", gaps)

    def test_unexpected_same_family_target_is_rejected(self) -> None:
        manifest = self.valid_manifest()
        family_uuid = self.gate.expected_family_uuid("mysql")
        manifest["same_family_library_set"].append({
            "target": "sbl_mysql_parser_undeclared_helper",
            "artifact": "lib/libsbl_mysql_parser_undeclared_helper.a",
            "owner": family_uuid,
        })
        _, gaps = self.gate.parse_package_identity("mysql", json.dumps(manifest))
        self.assertTrue(any("unexpected" in gap for gap in gaps), gaps)

    def test_empty_neutral_set_is_valid(self) -> None:
        manifest = self.valid_manifest()
        manifest["neutral_dependency_set"] = []
        _, gaps = self.gate.parse_package_identity("mysql", json.dumps(manifest))
        self.assertEqual([], gaps)

    def test_null_support_udr_omits_udr_target(self) -> None:
        manifest = self.valid_manifest()
        manifest["parser_support_udr_family_uuid"] = None
        manifest["same_family_library_set"] = [
            entry
            for entry in manifest["same_family_library_set"]
            if entry["target"] != "sbu_mysql_parser_support"
        ]
        _, gaps = self.gate.parse_package_identity("mysql", json.dumps(manifest))
        self.assertEqual([], gaps)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gate-script", required=True, type=pathlib.Path)
    parser.add_argument("--ownership-map", required=True, type=pathlib.Path)
    args = parser.parse_args()
    BinaryIsolationGateTests.gate = load_gate(args.gate_script.resolve())
    BinaryIsolationGateTests.ownership_map = args.ownership_map.resolve()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(BinaryIsolationGateTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
