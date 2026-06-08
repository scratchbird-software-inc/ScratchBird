#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Run ScratchBird AI component proof slices for CTest."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import subprocess
import sys
import time
import unittest
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


@dataclass(slots=True)
class CaseRecord:
    classname: str
    name: str
    status: str
    duration_sec: float
    message: str = ""


class RecordingResult(unittest.TestResult):
    def __init__(self) -> None:
        super().__init__()
        self.cases: list[CaseRecord] = []
        self._starts: dict[int, float] = {}

    def startTest(self, test: unittest.case.TestCase) -> None:  # type: ignore[override]
        self._starts[id(test)] = time.perf_counter()
        super().startTest(test)

    def _finish(self, test: unittest.case.TestCase, status: str, message: str = "") -> None:
        duration = time.perf_counter() - self._starts.pop(id(test), time.perf_counter())
        test_id = test.id()
        if "." in test_id:
            classname, name = test_id.rsplit(".", 1)
        else:
            classname, name = test.__class__.__name__, test_id
        self.cases.append(
            CaseRecord(
                classname=classname,
                name=name,
                status=status,
                duration_sec=duration,
                message=message,
            )
        )

    def addSuccess(self, test: unittest.case.TestCase) -> None:  # type: ignore[override]
        super().addSuccess(test)
        self._finish(test, "passed")

    def addFailure(self, test: unittest.case.TestCase, err: tuple[type[BaseException], BaseException, object]) -> None:  # type: ignore[override]
        super().addFailure(test, err)  # type: ignore[arg-type]
        self._finish(test, "failure", self._exc_info_to_string(err, test))  # type: ignore[arg-type]

    def addError(self, test: unittest.case.TestCase, err: tuple[type[BaseException], BaseException, object]) -> None:  # type: ignore[override]
        super().addError(test, err)  # type: ignore[arg-type]
        self._finish(test, "error", self._exc_info_to_string(err, test))  # type: ignore[arg-type]

    def addSkip(self, test: unittest.case.TestCase, reason: str) -> None:  # type: ignore[override]
        super().addSkip(test, reason)
        self._finish(test, "skipped", reason)


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def ensure_path(repo_root: Path) -> None:
    sys.dont_write_bytecode = True
    for path in (repo_root / "src", repo_root / "tools", repo_root):
        value = str(path)
        if value not in sys.path:
            sys.path.insert(0, value)


def configure_runtime_paths(output_dir: Path) -> None:
    runtime_dir = output_dir / "runtime"
    runtime_dir.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")
    os.environ.setdefault(
        "SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH",
        str(runtime_dir / "approval_ledger.json"),
    )
    os.environ.setdefault(
        "SCRATCHBIRD_AI_STRUCTURED_EVENT_LOG_PATH",
        str(runtime_dir / "structured_events.jsonl"),
    )
    os.environ.setdefault(
        "SCRATCHBIRD_AI_OPERATOR_BUNDLE_OUTPUT_DIR",
        str(runtime_dir / "operator_bundle"),
    )


def load_suite(repo_root: Path, patterns: list[str]) -> unittest.TestSuite:
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    tests_dir = repo_root / "tests"
    counter = 0
    for pattern in patterns:
        for test_path in sorted(tests_dir.glob(pattern)):
            module_name = f"_scratchbird_ai_ctest_{test_path.stem}_{counter}"
            counter += 1
            spec = importlib.util.spec_from_file_location(module_name, test_path)
            if spec is None or spec.loader is None:
                raise ImportError(f"unable to import {test_path}")
            module = importlib.util.module_from_spec(spec)
            sys.modules[module_name] = module
            spec.loader.exec_module(module)
            suite.addTests(loader.loadTestsFromModule(module))
    return suite


def write_json(path: Path, payload: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_junit(path: Path, suite_name: str, result: RecordingResult, duration_sec: float) -> None:
    testsuite = ET.Element(
        "testsuite",
        {
            "name": suite_name,
            "tests": str(result.testsRun),
            "failures": str(len(result.failures)),
            "errors": str(len(result.errors)),
            "skipped": str(len(result.skipped)),
            "time": f"{duration_sec:.6f}",
        },
    )
    for case in result.cases:
        testcase = ET.SubElement(
            testsuite,
            "testcase",
            {
                "classname": case.classname,
                "name": case.name,
                "time": f"{case.duration_sec:.6f}",
            },
        )
        if case.status == "failure":
            node = ET.SubElement(testcase, "failure", {"message": case.message.splitlines()[0] if case.message else "failure"})
            node.text = case.message
        elif case.status == "error":
            node = ET.SubElement(testcase, "error", {"message": case.message.splitlines()[0] if case.message else "error"})
            node.text = case.message
        elif case.status == "skipped":
            node = ET.SubElement(testcase, "skipped", {"message": case.message or "skipped"})
            node.text = case.message
    root = ET.Element("testsuites")
    root.append(testsuite)
    path.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(root).write(path, encoding="utf-8", xml_declaration=True)


def run_unittest_mode(repo_root: Path, output_dir: Path, mode: str) -> int:
    configure_runtime_paths(output_dir)
    patterns = ["test_*.py"]
    if mode == "optional-mcp":
        patterns = ["test_mcp_runtime_integration.py"]

    ensure_path(repo_root)
    suite = load_suite(repo_root, patterns)
    result = RecordingResult()
    started = time.perf_counter()
    suite.run(result)
    duration = time.perf_counter() - started

    failures = len(result.failures)
    errors = len(result.errors)
    skipped = len(result.skipped)
    status = "PASS" if failures == 0 and errors == 0 else "FAIL"
    classification = "tested"
    if mode == "optional-mcp" and skipped == result.testsRun and result.testsRun > 0:
        classification = "optional_runtime_unavailable"

    payload = {
        "generated_at_utc": utc_now(),
        "status": status,
        "mode": mode,
        "classification": classification,
        "tests_run": result.testsRun,
        "failures": failures,
        "errors": errors,
        "skipped": skipped,
        "duration_sec": round(duration, 6),
    }
    write_json(output_dir / f"{mode}.json", payload)
    write_junit(output_dir / f"{mode}.junit.xml", f"scratchbird_ai_{mode}", result, duration)

    if status != "PASS":
        for _, message in result.failures + result.errors:
            print(message, file=sys.stderr)
        return 1
    print(
        f"OK: ScratchBird AI {mode} gate {classification} "
        f"(tests={result.testsRun}, skipped={skipped})"
    )
    return 0


def run_live_native_classification(repo_root: Path, output_dir: Path) -> int:
    configure_runtime_paths(output_dir)
    live_dir = output_dir / "live_native_conformance"
    env = os.environ.copy()
    env["PYTHONPATH"] = str(repo_root / "src") + os.pathsep + env.get("PYTHONPATH", "")
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    result = subprocess.run(
        [
            sys.executable,
            str(repo_root / "tools" / "run_live_native_conformance.py"),
            "--repo-root",
            str(repo_root),
            "--output-dir",
            str(live_dir),
        ],
        cwd=repo_root,
        check=False,
        capture_output=True,
        text=True,
        env=env,
    )
    summary_path = live_dir / "summary.json"
    payload: dict[str, object] = {
        "generated_at_utc": utc_now(),
        "status": "FAIL",
        "classification": "missing_summary",
        "tool_returncode": result.returncode,
        "stdout": result.stdout[-2000:],
        "stderr": result.stderr[-2000:],
    }
    if summary_path.exists():
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        failed = [str(item) for item in summary.get("failed_checks", [])]
        expected_unconfigured = any("SCRATCHBIRD_AI_LIVE_NATIVE_ENABLED" in item for item in failed)
        if result.returncode == 0 or expected_unconfigured:
            payload.update(
                {
                    "status": "PASS",
                    "classification": "live_native_passed" if result.returncode == 0 else "live_native_unconfigured",
                    "summary_status": summary.get("status"),
                    "failed_checks": failed,
                }
            )
        else:
            payload.update(
                {
                    "classification": "unexpected_live_native_failure",
                    "summary_status": summary.get("status"),
                    "failed_checks": failed,
                }
            )

    write_json(output_dir / "live_native_classification.json", payload)
    if payload["status"] != "PASS":
        print(json.dumps(payload, indent=2, sort_keys=True), file=sys.stderr)
        return 1
    print(f"OK: live native classified ({payload['classification']})")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument(
        "--mode",
        choices=("unit", "optional-mcp", "live-native-classification"),
        required=True,
    )
    args = parser.parse_args()
    repo_root = Path(args.repo_root).resolve()
    output_dir = Path(args.output_dir).resolve()

    if args.mode in {"unit", "optional-mcp"}:
        return run_unittest_mode(repo_root, output_dir, args.mode)
    return run_live_native_classification(repo_root, output_dir)


if __name__ == "__main__":
    raise SystemExit(main())
