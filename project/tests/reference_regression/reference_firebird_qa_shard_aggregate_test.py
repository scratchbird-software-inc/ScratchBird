#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import copy
import hashlib
import json
import tempfile
import unittest
from pathlib import Path

import reference_firebird_qa_shard_aggregate as aggregate


def test_scope() -> dict[str, object]:
    entries = [
        {
            "ordinal": index,
            "path": f"tests/case_{index:02d}_test.py",
            "size_bytes": 1,
            "content_sha256": hashlib.sha256(str(index).encode()).hexdigest(),
        }
        for index in range(12)
    ]
    digest = hashlib.sha256()
    for entry in entries:
        digest.update(entry["path"].encode())
        digest.update(b"\0")
        digest.update(entry["content_sha256"].encode())
        digest.update(b"\0")
    return {
        "case_file_count": len(entries),
        "manifest_sha256": digest.hexdigest(),
        "shard_count": 12,
        "entries": entries,
    }


def executable(role: str) -> dict[str, object]:
    return {
        "role": role,
        "requested_path": f"/tools/{role}",
        "resolved_path": f"/tools/{role}",
        "is_file": True,
        "executable": True,
        "size_bytes": 42,
        "sha256": hashlib.sha256(role.encode()).hexdigest(),
    }


def tool_identities() -> dict[str, object]:
    return {
        "identity_schema": "scratchbird_replay_tool_identity_v1",
        "identity_complete": True,
        "scratchbird": {
            "server": executable("scratchbird_server"),
            "listener": executable("scratchbird_firebird_listener"),
            "parser_worker": executable("scratchbird_firebird_parser_worker"),
        },
        "firebird_tools": {
            tool: executable(f"firebird_{tool}")
            for tool in aggregate.REQUIRED_FIREBIRD_TOOLS
        },
        "firebird_tool_adapter": {
            "kind": "endpoint_syntax_rewrite_only",
            "fabricated_tool_output": False,
            "underlying_binaries": "firebird_tools",
        },
    }


def package_identities() -> dict[str, object]:
    result: dict[str, object] = {
        package: {
            "version": version,
            "version_source": "test_fixture",
            "origin": f"/python/{package}/__init__.py",
            "origin_sha256": hashlib.sha256(package.encode()).hexdigest(),
        }
        for package, version in {
            "pytest": "9.0.0",
            "firebird_qa": "0.21.0",
            "firebird_driver": "2.0.3",
        }.items()
    }
    result["python"] = executable("pytest_python")
    return result


def case(entry: dict[str, object]) -> dict[str, object]:
    relative_path = str(entry["path"])
    return {
        "case_id": aggregate.canonical_case_id(relative_path),
        "relative_path": relative_path,
        "manifest_ordinal": entry["ordinal"],
        "input_sha256": entry["content_sha256"],
        "status": "passed",
        "junit_valid": True,
        "harness_startup_failed": False,
        "startup_attempts": [{"attempt": 1, "status": "ready"}],
        "startup_retry_count": 0,
        "pytest_deselected_count": 0,
        "node_outcomes": {
            "collected": 1,
            "passed": 1,
            "failed": 0,
            "errors": 0,
            "skipped": 0,
            "expected_version_or_platform_deselected": 0,
            "expected_upstream_static_skipped": 0,
            "unexpected_skipped": 0,
            "xfailed": 0,
            "xpassed": 0,
        },
        "expected_exclusions": [],
        "expected_upstream_static_skips": [],
        "upstream_static_skip_source_error": "",
        "unexpected_skips": [],
        "xfails": [],
        "xpasses": [],
    }


def shard(scope: dict[str, object], index: int) -> dict[str, object]:
    entries = [
        entry
        for entry in scope["entries"]
        if int(entry["ordinal"]) % int(scope["shard_count"]) == index
    ]
    cases = [case(entry) for entry in entries]
    nodes = {key: 0 for key in aggregate.NODE_OUTCOME_KEYS}
    for result in cases:
        for key in nodes:
            nodes[key] += result["node_outcomes"][key]
    expected_upstream_static_skips = [
        audit
        for result in cases
        for audit in result["expected_upstream_static_skips"]
    ]
    return {
        "schema_version": "scratchbird_firebird_qa_replay_gate_v1",
        "gate": "reference_firebird_qa_replay_gate",
        "family": "firebird",
        "run_mode": "release-mandatory",
        "status": "passed",
        "scope_kind": "canonical_full",
        "canonical_scope_claimed": True,
        "assigned_scope_complete": True,
        "scope_complete": False,
        "completion_claim": "canonical_shard_evidence_requires_12_shard_aggregate",
        "shard": {
            "index": index,
            "count": 12,
            "assignment": "zero_based_manifest_ordinal_modulo_shard_count",
        },
        "canonical_scope": {
            "case_file_count": scope["case_file_count"],
            "manifest_sha256": scope["manifest_sha256"],
            "shard_count": 12,
        },
        "full_discovered_case_count": scope["case_file_count"],
        "discovered_case_count": len(cases),
        "passed_case_count": len(cases),
        "failed_case_count": 0,
        "skipped_case_count": 0,
        "semantic_failed_case_count": 0,
        "harness_failed_case_count": 0,
        "scope_policy_failed_case_count": 0,
        "startup_failure_event_count": 0,
        "startup_recovered_case_count": 0,
        "full_discovery_manifest": {
            "case_file_count": scope["case_file_count"],
            "manifest_sha256": scope["manifest_sha256"],
        },
        "node_outcomes": nodes,
        "expected_upstream_static_skipped": nodes[
            "expected_upstream_static_skipped"
        ],
        "pytest_deselected_count": 0,
        "expected_exclusions": [],
        "expected_upstream_static_skips": expected_upstream_static_skips,
        "pytest_scope_controls": {
            "plugin_autoload_disabled": True,
            "command_addopts_cleared": True,
            "exact_case_file_per_invocation": True,
        },
        "startup_policy": {
            "semantic_failure_count_excludes_harness_startup_failures": True,
        },
        "tool_identities": tool_identities(),
        "package_identities": package_identities(),
        "case_results": cases,
    }


class FirebirdQaShardAggregateTest(unittest.TestCase):
    def write(self, root: Path, name: str, payload: dict[str, object]) -> Path:
        path = root / name
        path.write_text(json.dumps(payload), encoding="utf-8")
        return path

    def full_evidence(
        self, root: Path, scope: dict[str, object]
    ) -> tuple[list[Path], list[dict[str, object]]]:
        payloads = [shard(scope, index) for index in range(12)]
        paths = [
            self.write(root, f"s{index:02d}.json", payload)
            for index, payload in enumerate(payloads)
        ]
        return paths, payloads

    def test_complete_unique_12_shards_pass(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            root = Path(raw_root)
            scope = test_scope()
            paths, _ = self.full_evidence(root, scope)
            result = aggregate.aggregate_evidence(paths, scope)
            self.assertEqual(result["status"], "passed", result["errors"])
            self.assertEqual(result["covered_case_count"], 12)
            self.assertEqual(result["node_outcomes"]["collected"], 12)

    def test_expected_upstream_static_skip_is_aggregated_without_case_exclusion(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            root = Path(raw_root)
            scope = test_scope()
            paths, payloads = self.full_evidence(root, scope)
            skipped_case = payloads[0]["case_results"][0]
            node_id = "qa.case_00_test::test_original"
            reason = "Covered by canonical successor"
            audit = {
                "case_id": skipped_case["case_id"],
                "node_id": node_id,
                "reason": reason,
                "source_line": 17,
            }
            skipped_case["node_outcomes"].update({
                "passed": 0,
                "skipped": 1,
                "expected_upstream_static_skipped": 1,
            })
            skipped_case["expected_upstream_static_skips"] = [audit]
            payloads[0]["node_outcomes"] = dict(skipped_case["node_outcomes"])
            payloads[0]["expected_upstream_static_skipped"] = 1
            payloads[0]["expected_upstream_static_skips"] = [audit]
            paths[0] = self.write(root, "s00.json", payloads[0])

            result = aggregate.aggregate_evidence(paths, scope)

            self.assertEqual(result["status"], "passed", result["errors"])
            self.assertEqual(result["expected_upstream_static_skipped"], 1)
            self.assertEqual(result["expected_upstream_static_skips"], [audit])
            self.assertEqual(
                result["case_outcomes"], {"passed": 12, "failed": 0, "skipped": 0}
            )

    def test_static_skip_audit_near_miss_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            root = Path(raw_root)
            scope = test_scope()
            paths, payloads = self.full_evidence(root, scope)
            skipped_case = payloads[0]["case_results"][0]
            malformed_audit = {
                "case_id": skipped_case["case_id"],
                "node_id": "qa.case_00_test::test_original",
                "reason": "dynamic_reason_variable",
                "source_line": "not-an-ast-line",
            }
            skipped_case["node_outcomes"].update({
                "passed": 0,
                "skipped": 1,
                "expected_upstream_static_skipped": 1,
            })
            skipped_case["expected_upstream_static_skips"] = [malformed_audit]
            payloads[0]["node_outcomes"] = dict(skipped_case["node_outcomes"])
            payloads[0]["expected_upstream_static_skipped"] = 1
            payloads[0]["expected_upstream_static_skips"] = [malformed_audit]
            paths[0] = self.write(root, "s00.json", payloads[0])

            result = aggregate.aggregate_evidence(paths, scope)

            self.assertEqual(result["status"], "failed")
            self.assertTrue(
                any("invalid expected static skip audit" in error for error in result["errors"])
            )

    def test_unexpected_dynamic_skip_cannot_claim_completion(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            root = Path(raw_root)
            scope = test_scope()
            paths, payloads = self.full_evidence(root, scope)
            skipped_case = payloads[0]["case_results"][0]
            skipped_case["node_outcomes"].update({
                "passed": 0,
                "skipped": 1,
                "unexpected_skipped": 1,
            })
            skipped_case["unexpected_skips"] = [{
                "node_id": "qa.case_00_test::test_original",
                "reason": "runtime pytest.skip",
            }]
            payloads[0]["node_outcomes"] = dict(skipped_case["node_outcomes"])
            paths[0] = self.write(root, "s00.json", payloads[0])

            result = aggregate.aggregate_evidence(paths, scope)

            self.assertEqual(result["status"], "failed")
            self.assertFalse(result["scope_complete"])
            self.assertTrue(
                any("unexpected skip/xfail/xpass" in error for error in result["errors"])
            )

    def test_missing_shard_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            root = Path(raw_root)
            scope = test_scope()
            paths, _ = self.full_evidence(root, scope)
            result = aggregate.aggregate_evidence(paths[:-1], scope)
            self.assertEqual(result["status"], "failed")
            self.assertTrue(any("exactly 12" in error for error in result["errors"]))

    def test_recovered_bounded_startup_retry_remains_valid_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            root = Path(raw_root)
            scope = test_scope()
            paths, payloads = self.full_evidence(root, scope)
            recovered = payloads[0]["case_results"][0]
            recovered["startup_attempts"] = [
                {
                    "attempt": 1,
                    "status": "startup_failed",
                    "component": "scratchbird_server",
                    "reason": "readiness timeout",
                    "returncode": -15,
                    "stdout_tail": "",
                    "stderr_tail": "busy",
                },
                {"attempt": 2, "status": "ready"},
            ]
            recovered["startup_retry_count"] = 1
            payloads[0]["startup_failure_event_count"] = 1
            payloads[0]["startup_recovered_case_count"] = 1
            paths[0] = self.write(root, "s00.json", payloads[0])
            result = aggregate.aggregate_evidence(paths, scope)
            self.assertEqual(result["status"], "passed", result["errors"])

    def test_diagnostic_scope_cannot_claim_aggregate_completion(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            root = Path(raw_root)
            scope = test_scope()
            paths, payloads = self.full_evidence(root, scope)
            payloads[0]["scope_kind"] = "diagnostic_subset"
            paths[0] = self.write(root, "s00.json", payloads[0])
            result = aggregate.aggregate_evidence(paths, scope)
            self.assertEqual(result["status"], "failed")
            self.assertTrue(any("diagnostic scope" in error for error in result["errors"]))

    def test_manifest_membership_and_xpass_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            root = Path(raw_root)
            scope = test_scope()
            paths, payloads = self.full_evidence(root, scope)
            bad = copy.deepcopy(payloads[0])
            bad_case = bad["case_results"][0]
            bad_case["relative_path"] = "tests/not_canonical_test.py"
            bad_case["node_outcomes"]["failed"] = 1
            bad_case["node_outcomes"]["passed"] = 0
            bad_case["node_outcomes"]["xpassed"] = 1
            bad_case["xpasses"] = [{"node_id": "x", "reason": "XPASS"}]
            bad["node_outcomes"] = bad_case["node_outcomes"]
            paths[0] = self.write(root, "s00.json", bad)
            result = aggregate.aggregate_evidence(paths, scope)
            self.assertEqual(result["status"], "failed")
            self.assertTrue(any("shard membership" in error for error in result["errors"]))
            self.assertTrue(any("skip/xfail/xpass" in error for error in result["errors"]))


if __name__ == "__main__":
    unittest.main()
