# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Parser tests only; these fixtures are not database benchmark evidence."""

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "transaction_phase_summary.py"
SPEC = importlib.util.spec_from_file_location("transaction_phase_summary", SCRIPT)
SUMMARY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUMMARY)


class TransactionPhaseSummaryTest(unittest.TestCase):
    def test_success_failure_and_nested_operations_remain_separate(self):
        report = SUMMARY.summarize([
            "operation_id=transaction.commit\tok=true\ttotal_us=100\tpersist_us=70\n",
            "operation_id=transaction.commit\tok=true\ttotal_us=200\n",
            "operation_id=transaction.commit\tok=false\ttotal_us=300\n",
            "operation_id=persist_inventory\tok=true\ttotal_us=60\n",
        ])
        self.assertEqual(report["record_count"], 4)
        success = next(row for row in report["operations"]
                       if row["operation_id"] == "transaction.commit" and row["ok"])
        self.assertEqual(success["phases"]["total_us"]["mean_us"], 150)
        self.assertEqual(success["phases"]["total_us"]["p95_us"], 200)
        self.assertEqual(success["phases"]["persist_us"]["count"], 1)
        self.assertFalse(report["interpretation"]["nested_layers_additive"])

    def test_malformed_empty_and_truncated_input_refused(self):
        for lines in [[], ["\n"],
                      ["operation_id=x\tok=true\ttotal_us=1"],
                      ["operation_id=x\tok=true\n"],
                      ["operation_id=x\tok=yes\ttotal_us=1\n"],
                      ["operation_id=x\tok=true\ttotal_us=1\ttotal_us=2\n"],
                      ["operation_id=x\tok=true\ttotal_us=-1\n"],
                      ["operation_id=x\tok=true\ttotal_us=NaN\n"],
                      [f"operation_id=x\tok=true\ttotal_us={1 << 64}\n"]]:
            with self.subTest(lines=lines), self.assertRaises(SUMMARY.TraceError):
                SUMMARY.summarize(lines)

    def test_limits_and_zero_timing(self):
        line = "operation_id=x\tok=true\ttotal_us=0\n"
        self.assertEqual(SUMMARY.summarize([line])["record_count"], 1)
        with self.assertRaises(SUMMARY.TraceError):
            SUMMARY.summarize([line, line], max_records=1)
        with self.assertRaises(SUMMARY.TraceError):
            SUMMARY.summarize([line], max_records=0)

    def test_cli_refuses_overwrite_and_invalid_second_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            valid, invalid, output = root / "valid.tsv", root / "bad.tsv", root / "out.json"
            valid.write_text("operation_id=x\tok=true\ttotal_us=1\n")
            invalid.write_text("truncated")
            result = subprocess.run([sys.executable, str(SCRIPT), str(valid),
                                     str(invalid), "--output", str(output)], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output.exists())
            command = [sys.executable, str(SCRIPT), str(valid), "--output", str(output)]
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
            original = output.read_bytes()
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertEqual(output.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
