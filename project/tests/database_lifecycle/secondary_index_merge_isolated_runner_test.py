# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
import subprocess
import time
import unittest
from pathlib import Path
from unittest.mock import patch

from secondary_index_merge_isolated_gate import run_case


class IsolatedMergeRunner(unittest.TestCase):
    def test_receipt_is_exact_and_private_artifacts_are_removed(self):
        for code, output, error, accepted in (
            (0, "secondary_merge_case=authoritative_merge passed\n", "", True),
            (1, "secondary_merge_case=authoritative_merge passed\n", "", False),
            (0, "", "", False),
            (0, "secondary_merge_case=malformed_payloads passed\n", "", False),
            (0, "secondary_merge_case=authoritative_merge passed\nextra\n", "", False),
            (0, "secondary_merge_case=authoritative_merge passed\n", "failure", False),
        ):
            with self.subTest(code=code, output=output, error=error):
                roots = []
                def child(command, **options):
                    root = Path(options["env"]["TMPDIR"])
                    self.assertEqual(str(root), options["env"]["TEMP"])
                    self.assertEqual(str(root), options["env"]["TMP"])
                    (root / "test.sbdb").write_bytes(b"owned failed-test artifact")
                    roots.append(root)
                    self.assertGreater(options["timeout"], 0)
                    self.assertLessEqual(options["timeout"], 90)
                    return subprocess.CompletedProcess(command, code, output, error)
                with patch("secondary_index_merge_isolated_gate.subprocess.run", side_effect=child):
                    if accepted:
                        self.assertEqual("authoritative_merge", run_case(
                            Path("probe"), "authoritative_merge", time.monotonic() + 10)[0])
                    else:
                        with self.assertRaises(RuntimeError):
                            run_case(Path("probe"), "authoritative_merge", time.monotonic() + 10)
                self.assertEqual(1, len(roots))
                self.assertFalse(roots[0].exists())

    def test_exhausted_deadline_does_not_start_another_node(self):
        with patch("secondary_index_merge_isolated_gate.subprocess.run") as child:
            with self.assertRaisesRegex(RuntimeError, "deadline exhausted"):
                run_case(Path("probe"), "authoritative_merge", time.monotonic() - 1)
            child.assert_not_called()

    def test_timeout_is_not_a_success_receipt(self):
        with patch("secondary_index_merge_isolated_gate.subprocess.run",
                   side_effect=subprocess.TimeoutExpired("probe", 1)):
            with self.assertRaises(subprocess.TimeoutExpired):
                run_case(Path("probe"), "authoritative_merge", time.monotonic() + 10)


if __name__ == "__main__":
    unittest.main()
