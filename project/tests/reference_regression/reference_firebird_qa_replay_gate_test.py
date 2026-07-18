#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import argparse
import os
import subprocess
import tempfile
import unittest
from unittest import mock
from pathlib import Path

import reference_firebird_qa_replay_gate as gate


class FirebirdQaReplayGateTest(unittest.TestCase):
    def test_discovery_manifest_is_content_and_path_bound(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            root = Path(raw_root)
            first = root / "tests" / "first_test.py"
            second = root / "tests" / "second_test.py"
            first.parent.mkdir(parents=True)
            first.write_text("def test_first(): pass\n", encoding="utf-8")
            second.write_text("def test_second(): pass\n", encoding="utf-8")

            before = gate.discovery_manifest(root, [first, second])
            repeated = gate.discovery_manifest(root, [first, second])
            self.assertEqual(before, repeated)
            self.assertEqual(before["case_file_count"], 2)

            second.write_text("def test_second(): assert True\n", encoding="utf-8")
            after = gate.discovery_manifest(root, [first, second])
            self.assertNotEqual(before["manifest_sha256"], after["manifest_sha256"])

    def test_junit_counts_make_skip_and_xfail_visible(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            junit = Path(raw_root) / "junit.xml"
            junit.write_text(
                "<testsuite>"
                "<testcase name='pass'/>"
                "<testcase name='fail'><failure/></testcase>"
                "<testcase name='error'><error/></testcase>"
                "<testcase name='xfail'><skipped type='pytest.xfail'/></testcase>"
                "</testsuite>",
                encoding="utf-8",
            )
            self.assertEqual(
                gate.junit_outcome_counts(junit),
                {
                    "collected": 4,
                    "passed": 1,
                    "failed": 1,
                    "errors": 1,
                    "skipped": 1,
                    "expected_version_or_platform_deselected": 0,
                    "expected_upstream_static_skipped": 0,
                    "unexpected_skipped": 0,
                    "xfailed": 1,
                    "xpassed": 0,
                },
            )

    def test_original_version_and_platform_deselection_is_audited(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            junit = Path(raw_root) / "junit.xml"
            junit.write_text(
                "<testsuite><testcase name='old-version'>"
                "<skipped type='pytest.skip' message='Skipped: Not for 5.0.0'>"
                "/qa/test_old.py:17: unrelated body text"
                "</skipped>"
                "</testcase></testsuite>",
                encoding="utf-8",
            )
            counts = gate.junit_outcome_counts(junit)
            self.assertEqual(counts["skipped"], 1)
            self.assertEqual(counts["expected_version_or_platform_deselected"], 1)
            self.assertEqual(counts["unexpected_skipped"], 0)
            self.assertEqual(counts["xfailed"], 0)

            evidence = gate.junit_outcome_evidence(junit)
            self.assertEqual(
                evidence["expected_exclusions"],
                [{
                    "node_id": "old-version",
                    "reason": "Skipped: Not for 5.0.0",
                    "excluded_target": "5.0.0",
                    "exclusion_kind": "version",
                }],
            )

    def test_literal_unconditional_static_skip_is_exactly_audited(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            root = Path(raw_root)
            source = root / "test_static.py"
            source.write_text(
                "import pytest\n"
                "\n"
                "@pytest.mark.skip(\"Covered by canonical successor\")\n"
                "def test_original():\n"
                "    pass\n",
                encoding="utf-8",
            )
            junit = root / "junit.xml"
            junit.write_text(
                "<testsuite><testcase classname='qa.test_static' "
                "name='test_original'>"
                "<skipped type='pytest.skip' "
                "message='Covered by canonical successor'>"
                "/qa/test_static.py:3: body text is not part of the message"
                "</skipped></testcase></testsuite>",
                encoding="utf-8",
            )

            evidence = gate.junit_outcome_evidence(junit, source)

            self.assertEqual(evidence["counts"]["skipped"], 1)
            self.assertEqual(
                evidence["counts"]["expected_upstream_static_skipped"], 1
            )
            self.assertEqual(evidence["counts"]["unexpected_skipped"], 0)
            self.assertEqual(
                evidence["expected_upstream_static_skips"],
                [{
                    "node_id": "qa.test_static::test_original",
                    "reason": "Covered by canonical successor",
                    "source_line": 3,
                }],
            )

    def test_junit_message_selects_one_of_multiple_literal_static_decorators(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            root = Path(raw_root)
            source = root / "test_static.py"
            source.write_text(
                "import pytest\n"
                "@pytest.mark.skip('Covered by broad successor')\n"
                "@pytest.mark.skip('Covered by exact successor')\n"
                "def test_original(): pass\n",
                encoding="utf-8",
            )
            junit = root / "junit.xml"
            junit.write_text(
                "<testsuite><testcase classname='qa.test_static' "
                "name='test_original'><skipped type='pytest.skip' "
                "message='Covered by exact successor'/></testcase></testsuite>",
                encoding="utf-8",
            )

            evidence = gate.junit_outcome_evidence(junit, source)

            self.assertEqual(
                evidence["counts"]["expected_upstream_static_skipped"], 1
            )
            self.assertEqual(
                evidence["expected_upstream_static_skips"][0]["source_line"], 3
            )

    def test_static_skip_near_misses_and_dynamic_forms_fail_closed(self) -> None:
        reason = "Covered by canonical successor"
        near_misses = {
            "dynamic_reason": (
                "import pytest\n"
                f"reason = {reason!r}\n"
                "@pytest.mark.skip(reason)\n"
                "def test_original(): pass\n"
            ),
            "skipif": (
                "import pytest\n"
                f"@pytest.mark.skipif(True, reason={reason!r})\n"
                "def test_original(): pass\n"
            ),
            "xfail": (
                "import pytest\n"
                f"@pytest.mark.xfail(reason={reason!r})\n"
                "def test_original(): pass\n"
            ),
            "runtime_skip": (
                "import pytest\n"
                "def test_original():\n"
                f"    pytest.skip({reason!r})\n"
            ),
            "aliased_marker": (
                "import pytest\n"
                "skip = pytest.mark.skip\n"
                f"@skip({reason!r})\n"
                "def test_original(): pass\n"
            ),
            "different_collected_node": (
                "import pytest\n"
                f"@pytest.mark.skip({reason!r})\n"
                "def test_other(): pass\n"
            ),
        }
        with tempfile.TemporaryDirectory() as raw_root:
            root = Path(raw_root)
            junit = root / "junit.xml"
            junit.write_text(
                "<testsuite><testcase classname='qa.test_static' "
                "name='test_original'><skipped type='pytest.skip' "
                f"message='{reason}'/></testcase></testsuite>",
                encoding="utf-8",
            )
            source = root / "test_static.py"
            for label, source_text in near_misses.items():
                with self.subTest(label=label):
                    source.write_text(source_text, encoding="utf-8")
                    evidence = gate.junit_outcome_evidence(junit, source)
                    self.assertEqual(
                        evidence["counts"]["expected_upstream_static_skipped"], 0
                    )
                    self.assertEqual(evidence["counts"]["unexpected_skipped"], 1)
                    self.assertEqual(evidence["expected_upstream_static_skips"], [])

            source.write_text(
                "import pytest\n"
                f"@pytest.mark.skip({reason!r})\n"
                "def test_original(): pass\n",
                encoding="utf-8",
            )
            junit.write_text(
                "<testsuite><testcase classname='qa.test_static' "
                "name='test_original'><skipped type='pytest.skip' "
                "message='Similar but not exact'/></testcase></testsuite>",
                encoding="utf-8",
            )
            evidence = gate.junit_outcome_evidence(junit, source)
            self.assertEqual(
                evidence["counts"]["expected_upstream_static_skipped"], 0
            )
            self.assertEqual(evidence["counts"]["unexpected_skipped"], 1)

    def test_skip_text_must_exactly_match_original_exclusion(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            junit = Path(raw_root) / "junit.xml"
            junit.write_text(
                "<testsuite><testcase name='hidden'>"
                "<skipped message='Not for 5.0.0 because the adapter is broken'>"
                "Not for 5.0.0"
                "</skipped>"
                "</testcase></testsuite>",
                encoding="utf-8",
            )
            counts = gate.junit_outcome_counts(junit)
            self.assertEqual(counts["expected_version_or_platform_deselected"], 0)
            self.assertEqual(counts["unexpected_skipped"], 1)

    def test_xpass_is_explicit_and_fail_closed_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            junit = Path(raw_root) / "junit.xml"
            junit.write_text(
                "<testsuite><testcase classname='qa.mod' name='xpass'>"
                "<failure message='[XPASS(strict)] known issue'/></testcase></testsuite>",
                encoding="utf-8",
            )
            evidence = gate.junit_outcome_evidence(junit)
            self.assertEqual(evidence["counts"]["failed"], 1)
            self.assertEqual(evidence["counts"]["xpassed"], 1)
            self.assertEqual(evidence["xpasses"][0]["node_id"], "qa.mod::xpass")

    def test_checked_canonical_scope_is_3003_content_bound_files(self) -> None:
        scope = gate.load_canonical_scope()
        self.assertEqual(scope["case_file_count"], 3003)
        self.assertEqual(scope["shard_count"], 12)
        self.assertEqual(len(scope["entries"]), 3003)
        self.assertEqual(
            scope["manifest_sha256"],
            "66cda67eab0fed5fbb57ab39626019f46e82a687c1af70a47efa908dece7c0a2",
        )

    def test_subset_override_is_rejected_outside_diagnostic_mode(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            qa_root = Path(raw_root)
            test = qa_root / "tests" / "one_test.py"
            test.parent.mkdir(parents=True)
            test.write_text("def test_one(): pass\n", encoding="utf-8")
            with mock.patch.dict(
                "os.environ", {"SB_REFERENCE_FIREBIRD_QA_TESTS": str(test)}
            ):
                with self.assertRaisesRegex(RuntimeError, "diagnostic-subset"):
                    gate.selected_tests(qa_root, qa_root, "release-mandatory")
                self.assertEqual(
                    gate.selected_tests(qa_root, qa_root, "diagnostic-subset"),
                    [test],
                )

    def test_gstat_wrapper_never_fabricates_success_output(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            root = Path(raw_root)
            real_bin = root / "real"
            real_bin.mkdir()
            gstat = real_bin / "gstat"
            gstat.write_text("#!/bin/sh\nexit 9\n", encoding="utf-8")
            gstat.chmod(0o755)
            wrapper_dir = gate.firebird_tool_bin_dir(root / "work", real_bin, 3055)
            self.assertIsNotNone(wrapper_dir)
            wrapper = (wrapper_dir / "gstat").read_text(encoding="utf-8")
            self.assertNotIn("Database header page information", wrapper)
            self.assertIn('exec "$real_tool" "${args[@]}"', wrapper)

    def test_pytest_deselection_summary_is_counted(self) -> None:
        self.assertEqual(
            gate.pytest_deselected_count("2 passed, 3 deselected in 0.1s"), 3
        )

    def test_server_startup_early_exit_reports_rc_and_stderr(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            root = Path(raw_root)
            server = root / "exit_server.sh"
            server.write_text(
                "#!/bin/sh\nprintf 'startup exploded\\n' >&2\nexit 23\n",
                encoding="utf-8",
            )
            server.chmod(0o755)
            args = argparse.Namespace(
                server=str(server), family="firebird", startup_timeout=1.0
            )
            with self.assertRaises(gate.smoke.OriginalToolStartupError) as caught:
                gate.smoke.start_server(args, root / "work")
            self.assertEqual(caught.exception.returncode, 23)
            self.assertIn("startup exploded", caught.exception.stderr_tail)

    def test_server_startup_timeout_reaps_unreturned_child(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            root = Path(raw_root)
            pid_file = root / "server.pid"
            server = root / "sleep_server.sh"
            server.write_text(
                "#!/bin/sh\n"
                f"printf '%s' \"$$\" > {gate.shlex_quote(str(pid_file))}\n"
                "exec sleep 30\n",
                encoding="utf-8",
            )
            server.chmod(0o755)
            args = argparse.Namespace(
                server=str(server), family="firebird", startup_timeout=0.1
            )
            with self.assertRaises(gate.smoke.OriginalToolStartupError) as caught:
                gate.smoke.start_server(args, root / "work")
            self.assertIn("timed out waiting", caught.exception.reason)
            pid = int(pid_file.read_text(encoding="utf-8"))
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)

    def test_listener_startup_early_exit_is_reaped_and_diagnostic(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            root = Path(raw_root)
            listener = root / "exit_listener.sh"
            listener.write_text(
                "#!/bin/sh\nprintf 'listener exploded\\n' >&2\nexit 24\n",
                encoding="utf-8",
            )
            listener.chmod(0o755)
            args = argparse.Namespace(
                listener=str(listener),
                parser_worker="/unused/parser",
                family="firebird",
                port=0,
                startup_timeout=1.0,
            )
            server_info = {
                "database": str(root / "unused.sbdb"),
                "endpoint": str(root / "unused.sock"),
            }
            with self.assertRaises(gate.smoke.OriginalToolStartupError) as caught:
                gate.smoke.start_listener(args, root / "work", server_info)
            self.assertEqual(caught.exception.returncode, 24)
            self.assertIn("listener exploded", caught.exception.stderr_tail)

    def test_listener_controller_uses_only_neutral_auth_envelope(self) -> None:
        class FakeListenerProcess:
            def poll(self) -> None:
                return None

        with tempfile.TemporaryDirectory() as raw_root:
            root = Path(raw_root)
            args = argparse.Namespace(
                listener="/fixture/SBgate",
                parser_worker="/fixture/sbp_firebird",
                family="firebird",
                port=3055,
                startup_timeout=1.0,
            )
            server_info = {
                "database": str(root / "fixture.sbdb"),
                "endpoint": str(root / "fixture.sbps.sock"),
            }
            hostile_compatibility_environment = {
                "SB_COMPATIBILITY_FIREBIRD_PASSWORD": "compatibility-password",
                "SB_REFERENCE_FIREBIRD_PASSWORD": "reference-password",
                "SB_COMPATIBILITY_FIREBIRD_VERIFIER": "compatibility-verifier",
                "SB_REFERENCE_FIREBIRD_VERIFIER": "reference-verifier",
                "SB_COMPATIBILITY_FIREBIRD_PRINCIPAL_UUID": "compatibility-uuid",
                "SB_REFERENCE_FIREBIRD_PRINCIPAL_UUID": "reference-uuid",
            }
            with (
                mock.patch.dict(os.environ, hostile_compatibility_environment, clear=True),
                mock.patch.object(
                    gate.smoke.subprocess,
                    "Popen",
                    return_value=FakeListenerProcess(),
                ) as popen,
                mock.patch.object(gate.smoke, "wait_for_tcp"),
            ):
                gate.smoke.start_listener(args, root / "work", server_info)

            child_environment = popen.call_args.kwargs["env"]
            self.assertEqual(
                child_environment["SB_COMPATIBILITY_AUTH_PASSWORD"],
                "local_password",
            )
            self.assertEqual(
                child_environment["SB_COMPATIBILITY_AUTH_VERIFIER"],
                gate.smoke.VERIFIER,
            )
            self.assertEqual(
                child_environment["SB_COMPATIBILITY_AUTH_PRINCIPAL_UUID"],
                gate.smoke.PRINCIPAL_UUID,
            )
            for compatibility_alias in hostile_compatibility_environment:
                self.assertNotIn(compatibility_alias, child_environment)

    def test_authenticated_readiness_probe_records_genuine_isql_identity(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            root = Path(raw_root)
            isql = root / "isql"
            isql.write_bytes(b"genuine-firebird-isql-test-double")
            observed: list[list[str]] = []

            def successful_run(command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
                observed.append(command)
                output = Path(command[command.index("-o") + 1])
                output.write_text("SB_QA_READY                    1\n", encoding="utf-8")
                return subprocess.CompletedProcess(command, 0, "", "")

            evidence = gate.firebird_authenticated_readiness_probe(
                isql,
                root / "lib",
                root,
                3055,
                5.0,
                run_fn=successful_run,
            )
            self.assertEqual(evidence["status"], "passed")
            self.assertEqual(evidence["command_id"], "firebird.isql.authenticated_readiness")
            self.assertEqual(evidence["tool_sha256"], gate.sha256_file(isql))
            self.assertEqual(observed[0][0], str(isql))
            self.assertEqual(observed[0][-1], "127.0.0.1/3055:default")

    def test_authenticated_readiness_failure_is_retryable_startup_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as raw_root:
            root = Path(raw_root)
            isql = root / "isql"
            isql.write_bytes(b"genuine-firebird-isql-test-double")

            def rejected_run(command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
                return subprocess.CompletedProcess(
                    command,
                    1,
                    "Your user name and password are not defined",
                    "",
                )

            with self.assertRaises(gate.smoke.OriginalToolStartupError) as caught:
                gate.firebird_authenticated_readiness_probe(
                    isql,
                    None,
                    root,
                    3055,
                    5.0,
                    run_fn=rejected_run,
                )
            self.assertEqual(
                caught.exception.component,
                "scratchbird_firebird_authenticated_readiness",
            )
            self.assertEqual(caught.exception.returncode, 1)
            self.assertIn("authentication", caught.exception.reason)

    def test_startup_retry_is_bounded_fresh_and_startup_only(self) -> None:
        class FakeProcess:
            def poll(self) -> None:
                return None

        first_server = FakeProcess()
        second_server = FakeProcess()
        second_listener = FakeProcess()
        failure = gate.smoke.OriginalToolStartupError(
            "scratchbird_listener",
            "timed out waiting for listener port",
            returncode=-15,
            stdout_tail="",
            stderr_tail="busy",
        )
        args = argparse.Namespace(startup_attempts=2, startup_retry_delay=0.25)
        with tempfile.TemporaryDirectory() as raw_root:
            work = Path(raw_root)
            sleeps: list[float] = []
            with (
                mock.patch.object(
                    gate.smoke,
                    "start_server",
                    side_effect=[
                        (first_server, {"database": "one", "endpoint": "one"}),
                        (second_server, {"database": "two", "endpoint": "two"}),
                    ],
                ) as start_server,
                mock.patch.object(
                    gate.smoke,
                    "start_listener",
                    side_effect=[failure, (second_listener, {"port": 3055})],
                ),
                mock.patch.object(gate.smoke, "stop_process") as stop_process,
            ):
                result = gate.start_case_runtime_with_retry(
                    args, work, sleep_fn=sleeps.append
                )
            self.assertEqual(result[0], second_server)
            self.assertEqual(result[2], second_listener)
            self.assertEqual(
                [record["status"] for record in result[-1]],
                ["startup_failed", "ready"],
            )
            self.assertEqual(sleeps, [0.25])
            self.assertEqual(start_server.call_count, 2)
            self.assertNotEqual(
                start_server.call_args_list[0].args[1],
                start_server.call_args_list[1].args[1],
            )
            stop_process.assert_any_call(first_server)

    def test_version_deselection_fails_closed(self) -> None:
        status, classification = gate.classify_returncode(
            5, "3 deselected in 0.01s"
        )
        self.assertEqual(status, "failed")
        self.assertEqual(classification, "scope_reduction_by_version_deselection")


if __name__ == "__main__":
    unittest.main()
