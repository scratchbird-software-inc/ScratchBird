# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Synthetic trace parser checks; not durability qualification."""
import io
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from transaction_sync_summary import summarize_sync, SyncTraceError


class TransactionSyncSummaryTest(unittest.TestCase):
    def parse(self, text):
        return summarize_sync(io.StringIO(text))

    def test_counts_paths_calls_failures_and_durations(self):
        report = self.parse("10  1780000000.000001 fsync(4</db with spaces>) = 0 <0.010000>\n"
                            "10  1780000000.010001 fsync(4</db with spaces>) = 0 <0.020000>\n"
                            "11  1780000000.020001 fdatasync(5</journal>) = -1 EIO (Input/output error) <0.003000>\n")
        self.assertEqual(report["syscall_count"], 3)
        self.assertEqual(report["failed_syscall_count"], 1)
        group = next(g for g in report["groups"] if g["ok"])
        self.assertEqual(group["count"], 2)
        self.assertAlmostEqual(group["duration_seconds_sum"], .03)

    def test_interleaved_resumption(self):
        report = self.parse("10  1780000000.000001 fsync(4</db> <unfinished ...>\n"
                            "11  1780000000.000002 fdatasync(5</j>) = 0 <0.001000>\n"
                            "10  1780000000.003000 <... fsync resumed>) = 0 <0.002999>\n")
        self.assertEqual(report["syscall_count"], 2)

    def test_rejects_missing_truncated_unknown_or_ambiguous_records(self):
        valid = "10  1780000000.000001 fsync(4</db>) = 0 <0.010000>\n"
        for text in ["", valid.rstrip(), "garbage\n", valid.replace("0.010000", "nan"),
                     valid.replace("4</db>", "4"), valid.replace("= 0", "= 1"),
                     "10  1780000000.000001 <... fsync resumed>) = 0 <0.010000>\n",
                     "10  1780000000.000001 fsync(4</db> <unfinished ...>\n",
                     "10  1780000000.000001 fsync(4</db> <unfinished ...>\n" + valid,
                     "10  1780000000.000001 fsync(4</db> <unfinished ...>\n"
                     "10  1780000000.010001 <... fdatasync resumed>) = 0 <0.010000>\n"]:
            with self.subTest(text=text), self.assertRaises(SyncTraceError):
                self.parse(text)


if __name__ == "__main__":
    unittest.main()
