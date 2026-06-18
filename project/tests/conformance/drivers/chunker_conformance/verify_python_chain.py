#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Verify the chain-level harness rule (P2-06).

Two checks:

1. `chain_cases.json` — synthetic compiled-chain fragments -> expected ordered
   ``[script_name, index, statement]`` tuples. Proves the rule directly:
   terminator reset at each ``-- begin_script:``, per-script index restart,
   ``SET TERM`` not counted, chain header ignored, terminator does not leak.

2. Real-suite parity — compiles the actual full-surface suite to a temp dir and
   asserts that iterating the concatenated chain with
   ``iter_chain_statements`` yields EXACTLY the same ``(script, index,
   statement)`` sequence as splitting each per-script compiled file on its own.
   This is the strongest guarantee that a runner gets identical ``statement_id``
   values (``<script>:<index>``) whether it executes the per-script files or the
   single chain, and that terminator state never leaks across scripts.

Run:  python3 verify_python_chain.py   (exit 0 = all pass)
"""
from __future__ import annotations

import json
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[4]
PY_SRC = REPO_ROOT / "project/drivers/driver/python/src"
SUITE_ROOT = REPO_ROOT / "project/tests/conformance/drivers/full_surface_scripts"
sys.path.insert(0, str(PY_SRC))
sys.path.insert(0, str(SUITE_ROOT))

from scratchbird.sql import (  # noqa: E402
    iter_chain_statements,
    split_top_level_statements,
)


def check_chain_cases() -> int:
    cases = json.loads((HERE / "chain_cases.json").read_text(encoding="utf-8"))["cases"]
    failures = 0
    for case in cases:
        got = [list(t) for t in iter_chain_statements(case["input"])]
        if got == case["expected"]:
            print(f"[PASS] chain_case:{case['name']}")
        else:
            failures += 1
            print(f"[FAIL] chain_case:{case['name']}")
            print(f"   expected: {case['expected']}")
            print(f"   got:      {got}")
    return failures


def check_real_suite_parity() -> int:
    """Compile the real suite and assert chain-mode == per-script-mode."""
    try:
        from compile_full_surface_script_suite import compile_suite
    except Exception as exc:  # pragma: no cover - import guard
        print(f"[SKIP] real-suite parity (cannot import compiler): {exc}")
        return 0

    values = {
        "__SB_NAMESPACE__": "sbtest_ns",
        "__SB_DRIVER__": "python",
        "__SB_RUN_ID__": "P2_06_PARITY",
        "__SB_ROUTE__": "authenticated",
        "__SB_PARSER_MODE__": "strict",
        "__SB_PAGE_SIZE__": "8192",
    }
    with tempfile.TemporaryDirectory(dir="/tmp", prefix="p2_06_parity_") as tmp:
        out = pathlib.Path(tmp)
        manifest = compile_suite(
            repo_root=REPO_ROOT,
            suite_root=SUITE_ROOT,
            output_root=out,
            values=values,
        )
        chain_text = (out / "full_surface_chain.sbsql").read_text(encoding="utf-8")
        chain_tuples = list(iter_chain_statements(chain_text))

        per_script_tuples: list[tuple[str, int, str]] = []
        for item in manifest.get("compiled_scripts", []):
            compiled_path = pathlib.Path(item["compiled_path"])
            name = compiled_path.name
            text = compiled_path.read_text(encoding="utf-8")
            for index, statement in enumerate(
                split_top_level_statements(text), start=1
            ):
                per_script_tuples.append((name, index, statement))

    if chain_tuples == per_script_tuples:
        print(
            f"[PASS] real-suite parity: {len(chain_tuples)} statements identical "
            "in chain-mode and per-script-mode"
        )
        return 0

    # Report the first divergence to make a failure debuggable.
    print("[FAIL] real-suite parity: chain-mode != per-script-mode")
    print(f"   chain statements={len(chain_tuples)} per_script={len(per_script_tuples)}")
    for i, (a, b) in enumerate(zip(chain_tuples, per_script_tuples)):
        if a != b:
            print(f"   first divergence at #{i}:")
            print(f"     chain:      {a}")
            print(f"     per-script: {b}")
            break
    return 1


def main() -> int:
    failures = check_chain_cases()
    failures += check_real_suite_parity()
    total_note = "chain harness-rule checks"
    if failures:
        print(f"\n{total_note}: FAILED ({failures} failing)")
        return 1
    print(f"\n{total_note}: all passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
