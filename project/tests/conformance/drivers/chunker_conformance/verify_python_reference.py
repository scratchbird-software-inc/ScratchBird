#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Verify the Python reference statement-chunker against the cross-driver fixture.

This is the reference verifier for `cases.json`. Each other driver MUST add an
equivalent test in its own language that loads `cases.json` and asserts its
splitter reproduces `expected` for every `input`.

Run:  python3 verify_python_reference.py   (exit 0 = all pass)
"""
from __future__ import annotations

import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[4]
PY_SRC = REPO_ROOT / "project/drivers/driver/python/src"
sys.path.insert(0, str(PY_SRC))

from scratchbird.sql import split_top_level_statements  # noqa: E402


def main() -> int:
    cases = json.loads((HERE / "cases.json").read_text(encoding="utf-8"))["cases"]
    failures = 0
    for case in cases:
        got = split_top_level_statements(case["input"])
        if got == case["expected"]:
            print(f"[PASS] {case['name']}")
        else:
            failures += 1
            print(f"[FAIL] {case['name']}")
            print(f"   expected: {case['expected']}")
            print(f"   got:      {got}")
    print(f"\n{len(cases) - failures}/{len(cases)} chunker conformance cases passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
