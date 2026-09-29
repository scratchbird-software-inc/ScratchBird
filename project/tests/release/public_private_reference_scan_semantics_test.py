# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
import importlib.util
from pathlib import Path
import tempfile
import unittest

path = Path(__file__).resolve().parents[2] / "tools/release/public_private_reference_scan.py"
spec = importlib.util.spec_from_file_location("reference_scan", path)
scanner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scanner)
metadata = "." + "git"
predicate = "any(part in {" + repr(metadata) + ", 'build'} for part in relative.parts)"


def source(condition=predicate, body="continue", prefix=""):
    return prefix + "for relative in paths:\n    if " + condition + ":\n        " + body + "\n"


class ScanSemanticsTest(unittest.TestCase):
    def flagged(self, text):
        return scanner.contains_banned_reference("git_metadata_reference", metadata,
                                                 scanner.python_git_dependency_text(text))

    def test_literal_directory_exclusion_is_not_content_dependency(self):
        for condition in (predicate, "unsafe_file or " + predicate,
                          "('é' == other) or " + predicate):
            with self.subTest(condition=condition):
                self.assertFalse(self.flagged(source(condition)))

    def test_actual_access_in_same_file_remains_a_failure(self):
        for access in ("open(" + repr(metadata) + ")\n",
                       "path = " + repr(metadata + "/config") + "\n"):
            self.assertTrue(self.flagged(source() + access))

    def test_non_exclusion_contexts_are_not_masked(self):
        for condition in ("not " + predicate, "enabled and " + predicate,
                          predicate.replace(" in ", " not in "),
                          predicate.replace("any(", "all("),
                          predicate.replace("relative.parts", "read_contents()"),
                          predicate.replace("relative.parts)", "relative.parts if enabled)"),
                          predicate.replace(repr(metadata), repr(metadata + "/config"))):
            with self.subTest(condition=condition):
                self.assertTrue(self.flagged(source(condition)))
        self.assertTrue(self.flagged(source(body="read_file(relative)")))
        self.assertTrue(self.flagged(source() + "    else:\n        continue\n"))

    def test_shadowed_any_and_malformed_python_fail_closed(self):
        for prefix in ("any = replacement\n", "from other import any\n",
                       "def any(values): return False\n"):
            self.assertTrue(self.flagged(source(prefix=prefix)))
        self.assertTrue(self.flagged(source() + "broken syntax @\n"))

    def test_project_scan_retains_other_private_reference_checks(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            candidate = root / "cleanup.py"
            candidate.write_text(source())
            self.assertEqual(scanner.scan(root), [])
            for label, needle in scanner.banned_needles():
                with self.subTest(label=label):
                    candidate.write_text(source() + "dependency = " + repr(needle) + "\n")
                    self.assertTrue(any(label in finding for finding in scanner.scan(root)))

    def test_non_python_files_do_not_receive_python_exclusions(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            (root / "cleanup.txt").write_text(source())
            self.assertTrue(any("git_metadata_reference" in finding for finding in scanner.scan(root)))


if __name__ == "__main__":
    unittest.main()
