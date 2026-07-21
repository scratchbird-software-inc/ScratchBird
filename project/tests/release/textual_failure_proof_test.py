#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Focused checks for non-binary GitHub Actions failure-proof staging."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "project/tools/release/stage_textual_failure_proof.py"
SPEC = importlib.util.spec_from_file_location("stage_textual_failure_proof", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
proof = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(proof)


class TextualFailureProofTest(unittest.TestCase):
    def test_payload_and_binary_tree_are_excluded(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-textual-proof-") as temp:
            root = Path(temp)
            source = root / "smoke"
            source.mkdir()
            (source / "result.log").write_text("smoke=failed\n", encoding="utf-8")
            (source / "report.json").write_text('{"status":"failed"}\n', encoding="utf-8")
            payload = source / "opt/ScratchBird/bin"
            payload.mkdir(parents=True)
            (payload / "SBsrv").write_bytes(b"\x7fELF\x02\x01payload")
            for directory in ("archive", "extract", "extraction"):
                extracted = source / directory
                extracted.mkdir()
                (extracted / "payload.log").write_text(
                    "must not publish\n", encoding="utf-8"
                )
            (source / "binary.txt").write_bytes(b"\xff\x00not utf8")
            (source / "elf.log").write_bytes(b"\x7fELF\x02\x01\0payload")
            (source / "oversized.log").write_bytes(
                b"x" * (proof.MAX_TEXTUAL_PROOF_BYTES + 1)
            )
            symlink = source / "result-link.log"
            try:
                symlink.symlink_to(source / "result.log")
            except OSError:
                # Some Windows test environments intentionally forbid
                # unprivileged link creation; the tool still rejects links on
                # every platform that can create one.
                symlink = None

            output = root / "proof"
            proof.stage([source], output)

            self.assertTrue((output / "source-1/result.log").is_file())
            self.assertTrue((output / "source-1/report.json").is_file())
            self.assertFalse((output / "source-1/opt").exists())
            self.assertFalse((output / "source-1/archive").exists())
            self.assertFalse((output / "source-1/extract").exists())
            self.assertFalse((output / "source-1/extraction").exists())
            self.assertFalse((output / "source-1/binary.txt").exists())
            self.assertFalse((output / "source-1/elf.log").exists())
            self.assertFalse((output / "source-1/oversized.log").exists())
            if symlink is not None:
                self.assertFalse((output / "source-1/result-link.log").exists())
            manifest = json.loads(
                (output / "FAILURE_PROOF_MANIFEST.json").read_text(encoding="utf-8")
            )
            self.assertEqual(
                "utf8_text_only_no_extracted_payload_or_binary_tree",
                manifest["policy"],
            )
            self.assertEqual(
                ["source-1/report.json", "source-1/result.log"],
                [row["path"] for row in manifest["files"]],
            )

    def test_missing_source_still_materializes_a_nonbinary_diagnostic(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-textual-proof-missing-") as temp:
            root = Path(temp)
            output = root / "proof"
            proof.stage([root / "does-not-exist"], output)
            self.assertTrue(
                (output / "source-1/NO_TEXTUAL_PROOF_AVAILABLE.txt").is_file()
            )
            files = [path for path in output.rglob("*") if path.is_file()]
            self.assertEqual(
                {
                    "FAILURE_PROOF_MANIFEST.json",
                    "source-1/NO_TEXTUAL_PROOF_AVAILABLE.txt",
                },
                {path.relative_to(output).as_posix() for path in files},
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
