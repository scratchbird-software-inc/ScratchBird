#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Source-oracle regressions, not runtime or cluster-execution evidence."""
import argparse
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

parser = argparse.ArgumentParser()
parser.add_argument("--repo-root", type=Path, required=True)
args, remaining = parser.parse_known_args()
ROOT = args.repo_root.resolve()
PROJECT = ROOT / "project"


def load(filename):
    path = PROJECT / "tools/release" / filename
    spec = importlib.util.spec_from_file_location("regression_" + path.stem, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class ReleaseContractOracles(unittest.TestCase):
    def test_generated_fixture_manifest_rejects_content_and_membership_drift(self):
        gate = load("public_generated_source_provenance.py")
        manifest = PROJECT / gate.DETERMINISTIC_MANIFEST
        expected = gate.read_manifest(manifest)
        changed = [dict(row) for row in expected]
        changed[0]['sha256'] = '0' * 64
        for actual in (changed, expected[1:]):
            with self.subTest(count=len(actual)), patch.object(gate, 'generated_artifact_rows', return_value=actual):
                self.refusal(lambda: gate.check_manifest_drift(ROOT, manifest),
                             'deterministic_artifact_manifest_drift')

    def test_diagnostic_matrix_uses_current_cluster_absence_contract(self):
        gate = load("public_diagnostic_matrix_generator.py")
        rows = gate.validate_rows(PROJECT)
        row = next(r for r in rows if r['area'] == 'cluster_boundary')
        self.assertEqual(row['code'], 'PROCESS.CLUSTER_PATH_ABSENT')
        self.assertEqual(row['message_key'], 'PROCESS.CLUSTER_PATH_ABSENT.safe')
        self.assertEqual(row['compatibility_status'], 'unsupported_stable')
        actual = gate.require_file
        # Both the production definition and its separate public test remain
        # required. An old refusal must not satisfy the current source oracle.
        for relative in (row['source_path'], row['public_test_path']):
            def obsolete(project_root, path):
                text = actual(project_root, path)
                if path == relative:
                    text = text.replace('PROCESS.CLUSTER_PATH_ABSENT',
                                        'SBLR.CLUSTER.SUPPORT_NOT_ENABLED')
                return text
            with self.subTest(path=relative), patch.object(gate, 'require_file', obsolete):
                self.refusal(lambda: gate.validate_rows(PROJECT),
                             'missing_token:' + relative + ':PROCESS.CLUSTER_PATH_ABSENT')

    def refusal(self, callback, reason):
        stream = io.StringIO()
        with contextlib.redirect_stderr(stream), self.assertRaises(SystemExit) as raised:
            callback()
        self.assertEqual(raised.exception.code, 1)
        self.assertIn(reason, stream.getvalue())

    def test_git_metadata_path_components(self):
        gate = load("public_doc_consistency_check.py")
        metadata = "." + "git"
        allowed = [metadata + "hub/workflows/build.yml", metadata + "ignore",
                   metadata + "attributes", metadata + "modules",
                   "https://example.org/project" + metadata,
                   "reference" + metadata + ".md", "my" + metadata + "/config"]
        for value in allowed:
            with self.subTest(value=value):
                gate.reject_private_reference(value, "independent_fixture")
        forbidden = [metadata, metadata + "/config", "repo/" + metadata + "/HEAD",
                     "repo\\" + metadata + "\\config", metadata.upper() + "/config",
                     "See `" + metadata + "/objects`.", "../" + metadata + "/objects",
                     "See /" + "home" + "/private/project",
                     "docs/" + "execution-plans/private.md"]
        for value in forbidden:
            with self.subTest(value=value):
                self.refusal(lambda: gate.reject_private_reference(value, "independent_fixture"),
                             "private_reference_recorded")

    def test_private_provider_linking_remains_refused(self):
        gate = load("public_cluster_boundary_cleanup_audit.py")
        gate.check_cmake_controls(ROOT, PROJECT)
        actual = gate.require_file
        def imported(path, *args):
            text = actual(path, *args)
            if path == PROJECT / "src/engine/internal_api/CMakeLists.txt":
                text += "\nadd_library(sb_cluster_provider UNKNOWN IMPORTED GLOBAL)\n"
            return text
        with patch.object(gate, "require_file", imported):
            self.refusal(lambda: gate.check_cmake_controls(ROOT, PROJECT),
                         "direct_private_provider_selection_forbidden")

    def test_direct_provider_cmake_guard_is_required(self):
        gate = load("public_platform_matrix_gate.py")
        gate.check_project_cmake(ROOT, PROJECT)
        actual = gate.require_file
        def unguarded(*args):
            return actual(*args).replace(
                "if(SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY OR SB_CLUSTER_PROVIDER_EXTERNAL_INCLUDE_DIR)",
                "if(FALSE)")
        with patch.object(gate, "require_file", unguarded):
            self.refusal(lambda: gate.check_project_cmake(ROOT, PROJECT),
                         "project_cmake_missing:if(SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY")

    def test_dedicated_node_default_remains_required(self):
        gate = load("public_default_config_check.py")
        request = argparse.Namespace(repo_root=ROOT, project_root=PROJECT)
        gate.build_evidence(request)
        actual = gate.require_file
        def shared(*args):
            return actual(*args).replace('std::string database_daemon_scope = "dedicated"',
                                        'std::string database_daemon_scope = "shared"')
        with patch.object(gate, "require_file", shared):
            self.refusal(lambda: gate.build_evidence(request), "database_daemon_scope")

    def test_matrix_cannot_claim_direct_private_provider_linking(self):
        gate = load("public_platform_matrix_gate.py")
        gate.check_matrix(ROOT, PROJECT)
        actual = gate.require_file
        def direct_link(path, *args):
            text = actual(path, *args)
            if path.name == "SUPPORTED_PLATFORM_TOOLCHAIN_MATRIX.md":
                text = text.replace('mode: "direct_private_provider_link_forbidden"',
                                    'mode: "cluster_external_boundary"')
            return text
        with patch.object(gate, "require_file", direct_link):
            self.refusal(lambda: gate.check_matrix(ROOT, PROJECT),
                         'matrix_missing:mode: "direct_private_provider_link_forbidden"')

    def test_obsolete_provider_manifest_is_refused(self):
        gate = load("public_doc_consistency_check.py")
        request = argparse.Namespace(repo_root=ROOT, project_root=PROJECT)
        gate.build_evidence(request)
        actual = gate.read_text
        def obsolete(path, *args):
            text = actual(path, *args)
            if path == ROOT / gate.PUBLIC_API_MANIFEST:
                text = text.replace("PROCESS.CLUSTER_PATH_ABSENT", "SBLR.CLUSTER.SUPPORT_NOT_ENABLED")
            return text
        with patch.object(gate, "read_text", obsolete):
            self.refusal(lambda: gate.build_evidence(request), "cluster_surface_refusal_code_drift")

    def test_agent_execution_refusal_is_not_a_handshake_diagnostic(self):
        gate = load("public_cluster_build_matrix_gate.py")
        gate.check_agent_cluster_boundary_proofs(PROJECT)
        actual = gate.require_file
        for filename in ("agent_cluster_provider_build_matrix_gate.cpp",
                         "agent_cluster_provider_boundary_gate.cpp",
                         "agent_cluster_leadership_boundary_gate.cpp"):
            def obsolete(path, *args):
                text = actual(path, *args)
                if path.name == filename:
                    text = text.replace("kClusterPathAbsentCode",
                                        "kClusterHandshakeStubCompileLinkOnlyCode")
                return text
            with self.subTest(filename=filename), patch.object(gate, "require_file", obsolete):
                self.refusal(lambda: gate.check_agent_cluster_boundary_proofs(PROJECT),
                             filename + ":agent_cluster_stub_boundary_missing:kClusterPathAbsentCode")
        # Handshake admission remains a separate non-execution contract.
        gate.check_handshake_contract(PROJECT)
        def missing_handshake(path, *args):
            return actual(path, *args).replace("kClusterHandshakeStubCompileLinkOnlyCode",
                                               "REMOVED_HANDSHAKE_REFUSAL")
        with patch.object(gate, "require_file", missing_handshake):
            self.refusal(lambda: gate.check_handshake_contract(PROJECT),
                         "cluster_provider_handshake_contract_missing:kClusterHandshakeStubCompileLinkOnlyCode")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *remaining])
