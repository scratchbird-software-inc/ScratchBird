#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate release-gating AI conformance artifacts from the current checkout."""

from __future__ import annotations

import argparse
import csv
import importlib.util
import json
import os
import statistics
import subprocess
import sys
import time
import unittest
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


@dataclass(slots=True)
class CaseRecord:
    classname: str
    name: str
    status: str
    duration_sec: float
    message: str = ""


@dataclass(slots=True)
class SuiteReport:
    tests_run: int
    failures: int
    errors: int
    skipped: int
    duration_sec: float
    cases: list[CaseRecord]

    @property
    def passed(self) -> bool:
        return self.failures == 0 and self.errors == 0


class RecordingResult(unittest.TestResult):
    def __init__(self) -> None:
        super().__init__()
        self.cases: list[CaseRecord] = []
        self._start_times: dict[int, float] = {}

    def startTest(self, test: unittest.case.TestCase) -> None:  # type: ignore[override]
        self._start_times[id(test)] = time.perf_counter()
        super().startTest(test)

    def _finish(self, test: unittest.case.TestCase, status: str, message: str = "") -> None:
        duration = time.perf_counter() - self._start_times.pop(id(test), time.perf_counter())
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

    def addFailure(self, test: unittest.case.TestCase, err: tuple[type[BaseException], BaseException, Any]) -> None:  # type: ignore[override]
        super().addFailure(test, err)
        self._finish(test, "failure", self._exc_info_to_string(err, test))

    def addError(self, test: unittest.case.TestCase, err: tuple[type[BaseException], BaseException, Any]) -> None:  # type: ignore[override]
        super().addError(test, err)
        self._finish(test, "error", self._exc_info_to_string(err, test))

    def addSkip(self, test: unittest.case.TestCase, reason: str) -> None:  # type: ignore[override]
        super().addSkip(test, reason)
        self._finish(test, "skipped", reason)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate ScratchBird AI conformance artifacts")
    parser.add_argument(
        "--repo-root",
        default=".",
        help="Repository root containing src/, tests/, tools/, and docs/",
    )
    parser.add_argument(
        "--artifact-root",
        default=None,
        help=(
            "Directory where generated artifacts are written. Defaults to "
            "<repo-root>/artifacts for standalone developer runs."
        ),
    )
    return parser.parse_args()


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def ensure_sys_path(repo_root: Path) -> None:
    sys.dont_write_bytecode = True
    src_path = repo_root / "src"
    tools_path = repo_root / "tools"
    for entry in (str(repo_root), str(src_path), str(tools_path)):
        if entry not in sys.path:
            sys.path.insert(0, entry)


def git_commit(repo_root: Path) -> str:
    try:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=repo_root,
            check=True,
            capture_output=True,
            text=True,
        )
    except subprocess.CalledProcessError:
        return "uncommitted"
    return result.stdout.strip() or "uncommitted"


def build_env(repo_root: Path, runtime_root: Path | None = None) -> dict[str, str]:
    env = os.environ.copy()
    existing = env.get("PYTHONPATH", "")
    src_path = str(repo_root / "src")
    env["PYTHONPATH"] = src_path if not existing else src_path + os.pathsep + existing
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    if runtime_root is not None:
        runtime_root.mkdir(parents=True, exist_ok=True)
        env.setdefault("SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH", str(runtime_root / "approval_ledger.json"))
        env.setdefault("SCRATCHBIRD_AI_STRUCTURED_EVENT_LOG_PATH", str(runtime_root / "structured_events.jsonl"))
        env.setdefault("SCRATCHBIRD_AI_OPERATOR_BUNDLE_OUTPUT_DIR", str(runtime_root / "operator_bundle"))
    return env


def run_command(repo_root: Path, args: list[str], runtime_root: Path | None = None) -> tuple[bool, str]:
    result = subprocess.run(
        args,
        cwd=repo_root,
        check=False,
        capture_output=True,
        text=True,
        env=build_env(repo_root, runtime_root=runtime_root),
    )
    output = (result.stdout + result.stderr).strip()
    if len(output) > 2000:
        output = output[:2000] + "\n...<truncated>"
    return result.returncode == 0, output


def load_suite(repo_root: Path, patterns: list[str]) -> unittest.TestSuite:
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    tests_dir = repo_root / "tests"
    module_counter = 0
    for pattern in patterns:
        for test_path in sorted(tests_dir.glob(pattern)):
            module_name = f"_scratchbird_ai_{test_path.stem}_{module_counter}"
            module_counter += 1
            spec = importlib.util.spec_from_file_location(module_name, test_path)
            if spec is None or spec.loader is None:
                raise ImportError(f"Unable to load test module from {test_path}")
            module = importlib.util.module_from_spec(spec)
            sys.modules[module_name] = module
            spec.loader.exec_module(module)
            suite.addTests(loader.loadTestsFromModule(module))
    return suite


def run_suite(repo_root: Path, patterns: list[str]) -> SuiteReport:
    suite = load_suite(repo_root, patterns)
    result = RecordingResult()
    started = time.perf_counter()
    suite.run(result)
    duration = time.perf_counter() - started
    return SuiteReport(
        tests_run=result.testsRun,
        failures=len(result.failures),
        errors=len(result.errors),
        skipped=len(result.skipped),
        duration_sec=duration,
        cases=result.cases,
    )


def write_junit(path: Path, suite_name: str, report: SuiteReport) -> None:
    testsuite = ET.Element(
        "testsuite",
        {
            "name": suite_name,
            "tests": str(report.tests_run),
            "failures": str(report.failures),
            "errors": str(report.errors),
            "skipped": str(report.skipped),
            "time": f"{report.duration_sec:.6f}",
        },
    )
    for case in report.cases:
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


def check_line_contains(path: Path, needle: str) -> tuple[bool, str]:
    text = path.read_text(encoding="utf-8")
    return (needle in text, needle)


def check_payload(name: str, passed: bool, detail: str = "") -> tuple[str, bool, str]:
    return name, passed, detail


def artifact_payload(
    *,
    generated_at_utc: str,
    git_sha: str,
    checks: list[tuple[str, bool, str]],
    extra: dict[str, Any] | None = None,
) -> dict[str, Any]:
    failed = []
    for name, passed, detail in checks:
        if not passed:
            failed.append(f"{name}: {detail}" if detail else name)
    payload: dict[str, Any] = {
        "generated_at_utc": generated_at_utc,
        "git_commit": git_sha,
        "status": "PASS" if not failed else "FAIL",
        "check_count": len(checks),
        "passed_checks": len(checks) - len(failed),
        "failed_checks": failed,
    }
    if extra:
        payload.update(extra)
    return payload


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def security_context() -> dict[str, Any]:
    return {
        "tenant_id": "tenant_a",
        "actor_id": "actor_a",
        "roles": ["analyst"],
        "session_id": "sess_1",
        "context_version": 1,
    }


def build_store() -> Any:
    from scratchbird_ai.retrieval import InMemoryRetrievalStore

    store = InMemoryRetrievalStore()
    store.add_embeddings(
        index_id="idx_docs",
        dimension=3,
        records=[
            {
                "vector_id": "doc-1#1",
                "embedding": [0.1, 0.2, 0.3],
                "metadata": {"document_id": "doc-1", "text": "north overdue invoice"},
            },
            {
                "vector_id": "doc-2#1",
                "embedding": [0.3, 0.1, 0.0],
                "metadata": {"document_id": "doc-2", "text": "south paid invoice"},
            },
            {
                "vector_id": "doc-3#1",
                "embedding": [0.2, 0.2, 0.2],
                "metadata": {"document_id": "doc-3", "text": "north invoice aging"},
            },
        ],
        security_context=security_context(),
    )
    return store


def build_retrieval_service() -> Any:
    from scratchbird_ai.service import build_default_service

    service = build_default_service()
    service.add_embeddings(
        index_id="idx_docs",
        dimension=3,
        records=[
            {
                "vector_id": "doc-1#1",
                "embedding": [0.1, 0.2, 0.3],
                "metadata": {"document_id": "doc-1", "text": "north overdue invoice"},
            },
            {
                "vector_id": "doc-2#1",
                "embedding": [0.3, 0.1, 0.0],
                "metadata": {"document_id": "doc-2", "text": "south paid invoice"},
            },
            {
                "vector_id": "doc-3#1",
                "embedding": [0.2, 0.2, 0.2],
                "metadata": {
                    "document_id": "doc-3",
                    "text": "north invoice aging",
                    "status": "OPEN",
                },
            },
        ],
        security_context=security_context(),
    )
    service.create_vector_index(
        index_id="idx_managed_docs",
        dimension=3,
        security_context=security_context(),
        profile_id="engine_managed_retrieval_v0",
    )
    service.add_embeddings(
        index_id="idx_managed_docs",
        dimension=3,
        records=[
            {
                "vector_id": "doc-managed-1#1",
                "embedding": [0.1, 0.2, 0.3],
                "metadata": {
                    "document_id": "doc-managed-1",
                    "text": "north overdue invoice",
                    "status": "OVERDUE",
                },
            },
            {
                "vector_id": "doc-managed-2#1",
                "embedding": [0.3, 0.1, 0.0],
                "metadata": {
                    "document_id": "doc-managed-2",
                    "text": "south paid invoice",
                    "status": "PAID",
                },
            },
        ],
        security_context=security_context(),
    )
    return service


def percentile_ms(samples_ms: list[float], q: float) -> float:
    if not samples_ms:
        return 0.0
    if len(samples_ms) == 1:
        return samples_ms[0]
    ordered = sorted(samples_ms)
    index = int(round((len(ordered) - 1) * q))
    return ordered[index]


def generate(repo_root: Path, artifact_root: Path | None = None) -> int:
    ensure_sys_path(repo_root)
    git_sha = git_commit(repo_root)
    generated_at = utc_now()
    artifacts_base = artifact_root if artifact_root is not None else repo_root / "artifacts"
    artifacts_root = artifacts_base / "ai_conformance"
    runtime_root = artifacts_base / "runtime"
    os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")
    os.environ.setdefault("SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH", str(runtime_root / "approval_ledger.json"))
    os.environ.setdefault("SCRATCHBIRD_AI_STRUCTURED_EVENT_LOG_PATH", str(runtime_root / "structured_events.jsonl"))
    os.environ.setdefault("SCRATCHBIRD_AI_OPERATOR_BUNDLE_OUTPUT_DIR", str(runtime_root / "operator_bundle"))

    from scratchbird_ai.audit_bundle import (
        ATTESTATION_OUTCOMES,
        REPLAY_OUTCOMES,
        create_audit_bundle,
        create_bundle_attestation,
        replay_validate_bundle,
        verify_bundle_attestation,
    )
    from scratchbird_ai.execution_mode import ApprovalEvidence, evaluate_execution_mode, validate_transition
    from scratchbird_ai.framework_adapters import (
        LangChainAdapter,
        LlamaIndexAdapter,
        SemanticKernelAdapter,
    )
    from scratchbird_ai.plan_introspection import build_plan_response, compute_plan_hash
    from scratchbird_ai.service import build_default_service
    from scratchbird_ai.cluster_routing import (
        ClusterNode,
        ClusterShard,
        ClusterTopology,
        distributed_vector_search,
        route_query,
    )

    generated_files: list[Path] = []

    def seed_framework_service(service: Any) -> None:
        service.add_embeddings(
            index_id="idx_framework_docs",
            dimension=3,
            records=[
                {
                    "vector_id": "doc-1#1",
                    "embedding": [0.1, 0.2, 0.3],
                    "metadata": {"document_id": "doc-1", "text": "north overdue invoice"},
                },
                {
                    "vector_id": "doc-2#1",
                    "embedding": [0.3, 0.1, 0.0],
                    "metadata": {"document_id": "doc-2", "text": "south paid invoice"},
                },
                {
                    "vector_id": "doc-3#1",
                    "embedding": [0.2, 0.2, 0.2],
                    "metadata": {"document_id": "doc-3", "text": "north invoice aging"},
                },
            ],
            security_context=security_context(),
        )

    def save(rel_path: str, payload: dict[str, Any]) -> None:
        path = artifacts_root / rel_path
        write_json(path, payload)
        generated_files.append(path)

    def save_csv(rel_path: str, rows: list[list[Any]]) -> None:
        path = artifacts_root / rel_path
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.writer(handle, lineterminator="\n")
            writer.writerows(rows)
        generated_files.append(path)

    def save_junit(rel_path: str, suite_name: str, report: SuiteReport) -> None:
        path = artifacts_root / rel_path
        write_junit(path, suite_name, report)
        generated_files.append(path)

    full_suite = run_suite(repo_root, ["test_*.py"])
    cap_ok, cap_output = run_command(
        repo_root,
        [sys.executable, "tools/validate_capability_matrix.py"],
        runtime_root=runtime_root,
    )
    smoke_ok, smoke_output = run_command(
        repo_root,
        [sys.executable, "tools/smoke_http_contract.py", "--mode", "selftest"],
        runtime_root=runtime_root,
    )
    baseline_checks = [
        check_payload("unit_suite", full_suite.passed, f"tests_run={full_suite.tests_run}"),
        check_payload("capability_matrix_validator", cap_ok, cap_output),
        check_payload("http_contract_selftest", smoke_ok, smoke_output),
        check_payload(
            "current_status_doc_present",
            (repo_root / "docs" / "status" / "EARLY_BETA_STATUS_2026-03-07.md").exists(),
            "docs/status/EARLY_BETA_STATUS_2026-03-07.md",
        ),
    ]
    save(
        "01/summary.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=baseline_checks,
            extra={
                "suite": "baseline_readiness",
                "tests_run": full_suite.tests_run,
                "duration_sec": round(full_suite.duration_sec, 6),
                "commands": {
                    "capability_matrix_validator": cap_output,
                    "http_contract_selftest": smoke_output,
                },
            },
        ),
    )

    http_suite = run_suite(
        repo_root,
        [
            "test_http_adapters.py",
            "test_http_bridge.py",
            "test_http_service_integration.py",
            "test_http_resilience.py",
        ],
    )
    save_junit("02/test_report.junit.xml", "http_runtime_contract", http_suite)
    save(
        "02/adapter_parity.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=[check_payload("http_runtime_test_suite", http_suite.passed, f"tests_run={http_suite.tests_run}")],
            extra={
                "suite": "http_runtime_contract",
                "tests_run": http_suite.tests_run,
                "duration_sec": round(http_suite.duration_sec, 6),
            },
        ),
    )

    service_suite = run_suite(
        repo_root,
        [
            "test_service.py",
            "test_compile_repair.py",
            "test_router.py",
            "test_policy.py",
            "test_settings.py",
        ],
    )
    save_junit("03/test_report.junit.xml", "service_surface_contract", service_suite)
    service = build_default_service()
    capabilities = service.get_capabilities()
    service_checks = [
        check_payload("service_surface_test_suite", service_suite.passed, f"tests_run={service_suite.tests_run}"),
        check_payload(
            "canonical_tool_catalog_present",
            set(capabilities["supports"]["canonical_tools"]) >= {
                "get_capabilities",
                "list_dialects",
                "list_schemas",
                "list_tables",
                "describe_table",
                "execute_readonly_query",
                "execute_mutation",
                "explain_query",
                "vector_search",
                "hybrid_search",
            },
            json.dumps(capabilities["supports"]["canonical_tools"]),
        ),
        check_payload(
            "native_only_policy_exposed",
            service.list_dialects() == ["native"],
            json.dumps(service.list_dialects()),
        ),
    ]
    save(
        "03/service_surface.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=service_checks,
            extra={
                "suite": "service_surface_contract",
                "tests_run": service_suite.tests_run,
                "duration_sec": round(service_suite.duration_sec, 6),
                "adapter_mode": capabilities["adapter_mode"],
                "tool_schema_version": capabilities["tool_schema_version"],
            },
        ),
    )

    retrieval_suite = run_suite(repo_root, ["test_retrieval.py"])
    retrieval_service = build_retrieval_service()
    vector_result = retrieval_service.vector_search(
        index_id="idx_docs",
        query_embedding=[0.1, 0.2, 0.3],
        top_k=3,
        filters={},
        include_vectors=False,
        security_context=security_context(),
    )
    managed_vector_result = retrieval_service.vector_search(
        index_id="idx_managed_docs",
        query_embedding=[0.1, 0.2, 0.3],
        top_k=3,
        filters={"status": "OVERDUE"},
        include_vectors=False,
        security_context=security_context(),
    )
    vector_checks = [
        check_payload("retrieval_test_suite", retrieval_suite.passed, f"tests_run={retrieval_suite.tests_run}"),
        check_payload(
            "vector_top_hit_expected",
            vector_result["results"][0]["vector_id"] == "doc-1#1",
            json.dumps(vector_result["results"][0]),
        ),
        check_payload("rls_applied", bool(vector_result["rls_applied"]), json.dumps(vector_result)),
        check_payload(
            "managed_profile_available",
            managed_vector_result["index"]["profile_id"] == "engine_managed_retrieval_v0",
            json.dumps(managed_vector_result["index"], sort_keys=True),
        ),
        check_payload(
            "managed_vector_top_hit_expected",
            managed_vector_result["results"][0]["vector_id"] == "doc-managed-1#1",
            json.dumps(managed_vector_result["results"][0]),
        ),
    ]
    benchmark_rows = [["scenario", "iterations", "mean_ms", "p95_ms", "top_hit"]]
    helper_samples_ms: list[float] = []
    for _ in range(25):
        started = time.perf_counter()
        helper_outcome = retrieval_service.vector_search(
            index_id="idx_docs",
            query_embedding=[0.1, 0.2, 0.3],
            top_k=3,
            filters={},
            include_vectors=False,
            security_context=security_context(),
        )
        helper_samples_ms.append((time.perf_counter() - started) * 1000.0)
    benchmark_rows.append(
        [
            "helper_vector_search",
            len(helper_samples_ms),
            f"{statistics.mean(helper_samples_ms):.6f}",
            f"{percentile_ms(helper_samples_ms, 0.95):.6f}",
            helper_outcome["results"][0]["vector_id"],
        ]
    )
    managed_samples_ms: list[float] = []
    for _ in range(25):
        started = time.perf_counter()
        managed_outcome = retrieval_service.vector_search(
            index_id="idx_managed_docs",
            query_embedding=[0.1, 0.2, 0.3],
            top_k=3,
            filters={"status": "OVERDUE"},
            include_vectors=False,
            security_context=security_context(),
        )
        managed_samples_ms.append((time.perf_counter() - started) * 1000.0)
    benchmark_rows.append(
        [
            "managed_vector_search",
            len(managed_samples_ms),
            f"{statistics.mean(managed_samples_ms):.6f}",
            f"{percentile_ms(managed_samples_ms, 0.95):.6f}",
            managed_outcome["results"][0]["vector_id"],
        ]
    )
    save_csv("04/benchmark.csv", benchmark_rows)
    save(
        "04/vector_api_report.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=vector_checks,
            extra={
                "tests_run": retrieval_suite.tests_run,
                "duration_sec": round(retrieval_suite.duration_sec, 6),
                "top_hit": vector_result["results"][0]["vector_id"],
                "managed_top_hit": managed_vector_result["results"][0]["vector_id"],
                "profiles": [
                    vector_result["index"]["profile_id"],
                    managed_vector_result["index"]["profile_id"],
                ],
            },
        ),
    )

    managed_index_descriptor = retrieval_service.describe_vector_index(
        index_id="idx_managed_docs",
        security_context=security_context(),
    )["index"]

    hybrid_result = retrieval_service.hybrid_search(
        dialect="native",
        query_text="north overdue invoice",
        query_embedding=[0.1, 0.2, 0.3],
        vector_index_id="idx_docs",
        sql_filter={"metadata": {"document_id": "doc-1"}},
        weights={"vector": 0.6, "lexical": 0.3, "structured": 0.1},
        top_k=5,
        security_context=security_context(),
    )
    managed_hybrid_result = retrieval_service.hybrid_search(
        dialect="native",
        query_text="north overdue invoice",
        query_embedding=[0.1, 0.2, 0.3],
        vector_index_id="idx_managed_docs",
        sql_filter={"where": "status = 'OVERDUE'"},
        weights={"vector": 0.6, "lexical": 0.3, "structured": 0.1},
        top_k=5,
        security_context=security_context(),
    )
    helper_where_denied = False
    helper_where_error = ""
    try:
        retrieval_service.hybrid_search(
            dialect="native",
            query_text="north overdue invoice",
            query_embedding=[0.1, 0.2, 0.3],
            vector_index_id="idx_docs",
            sql_filter={"where": "status = 'OVERDUE'"},
            weights={"vector": 0.6, "lexical": 0.3, "structured": 0.1},
            top_k=5,
            security_context=security_context(),
        )
    except Exception as exc:
        helper_where_denied = getattr(exc, "error_code", "") == "E_FILTER_PUSHDOWN_UNAVAILABLE"
        helper_where_error = str(getattr(exc, "error_code", exc))
    repeat_ids = []
    for _ in range(5):
        run = retrieval_service.hybrid_search(
            dialect="native",
            query_text="north overdue invoice",
            query_embedding=[0.1, 0.2, 0.3],
            vector_index_id="idx_docs",
            sql_filter={"metadata": {"document_id": "doc-1"}},
            weights={"vector": 0.6, "lexical": 0.3, "structured": 0.1},
            top_k=5,
            security_context=security_context(),
        )
        repeat_ids.append([row["document_id"] for row in run["results"]])
    hybrid_checks = [
        check_payload(
            "hybrid_top_hit_expected",
            hybrid_result["results"][0]["document_id"] == "doc-1",
            json.dumps(hybrid_result["results"][0]),
        ),
        check_payload(
            "hybrid_filter_pushdown_respected",
            all(row["document_id"] == "doc-1" for row in hybrid_result["results"]),
            json.dumps(hybrid_result["results"]),
        ),
        check_payload(
            "hybrid_ranking_deterministic",
            all(ids == repeat_ids[0] for ids in repeat_ids[1:]),
            json.dumps(repeat_ids),
        ),
        check_payload(
            "helper_where_pushdown_denied",
            helper_where_denied,
            helper_where_error,
        ),
        check_payload(
            "managed_where_pushdown_allowed",
            managed_hybrid_result["results"][0]["document_id"] == "doc-managed-1",
            json.dumps(managed_hybrid_result["results"][0]),
        ),
    ]
    save(
        "05/hybrid_report.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=hybrid_checks,
            extra={
                "weights": {"vector": 0.6, "lexical": 0.3, "structured": 0.1},
                "result_count": len(hybrid_result["results"]),
                "managed_profile": managed_index_descriptor["profile_id"],
                "managed_top_document_id": managed_hybrid_result["results"][0]["document_id"],
            },
        ),
    )
    save(
        "05/relevance_eval.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=[check_payload("expected_top_document", hybrid_result["results"][0]["document_id"] == "doc-1", json.dumps(hybrid_result["results"][0]))],
            extra={
                "query_text": "north overdue invoice",
                "top_document_id": hybrid_result["results"][0]["document_id"],
                "top_score": hybrid_result["results"][0]["scores"]["final"],
                "managed_query_text": "north overdue invoice",
                "managed_top_document_id": managed_hybrid_result["results"][0]["document_id"],
                "managed_top_score": managed_hybrid_result["results"][0]["scores"]["final"],
            },
        ),
    )

    tool_suite = run_suite(repo_root, ["test_tool_schema.py"])
    compat_checks = [
        check_payload("tool_schema_test_suite", tool_suite.passed, f"tests_run={tool_suite.tests_run}"),
        check_payload(
            "legacy_mode_alias_declared",
            capabilities["supports"]["legacy_mode_aliases"] == {
                "read_only": "ai_analysis",
                "mutation_with_approval": "ai_mutation_pending_approval",
            },
            json.dumps(capabilities["supports"]["legacy_mode_aliases"], sort_keys=True),
        ),
        check_payload(
            "canonical_modes_declared",
            capabilities["supports"]["canonical_execution_modes"] == [
                "ai_analysis",
                "ai_mutation_pending_approval",
                "ai_mutation_approved",
            ],
            json.dumps(capabilities["supports"]["canonical_execution_modes"]),
        ),
    ]
    save(
        "06/schema_report.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=[check_payload("tool_schema_test_suite", tool_suite.passed, f"tests_run={tool_suite.tests_run}")],
            extra={
                "tests_run": tool_suite.tests_run,
                "duration_sec": round(tool_suite.duration_sec, 6),
                "tool_schema_version": capabilities["tool_schema_version"],
            },
        ),
    )
    save(
        "06/compat_report.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=compat_checks,
            extra={
                "canonical_tools": capabilities["supports"]["canonical_tools"],
            },
        ),
    )

    plan_suite = run_suite(repo_root, ["test_plan_introspection.py"])
    operator_tree_a = {
        "operator_id": "root",
        "operator_type": "Join",
        "children": [
            {"operator_id": "b", "operator_type": "Scan", "children": []},
            {"operator_id": "a", "operator_type": "Scan", "children": []},
        ],
    }
    operator_tree_b = {
        "operator_id": "root",
        "operator_type": "Join",
        "children": [
            {"operator_id": "a", "operator_type": "Scan", "children": []},
            {"operator_id": "b", "operator_type": "Scan", "children": []},
        ],
    }
    plan_hash_a = compute_plan_hash(
        dialect="native",
        normalized_query="SELECT * FROM t",
        operator_tree=operator_tree_a,
        rls_policy_ids=["p2", "p1"],
        predicate_hash="pred123",
        planner_version="v1",
    )
    plan_hash_b = compute_plan_hash(
        dialect="native",
        normalized_query="SELECT * FROM t",
        operator_tree=operator_tree_b,
        rls_policy_ids=["p1", "p2"],
        predicate_hash="pred123",
        planner_version="v1",
    )
    diff_hash = compute_plan_hash(
        dialect="native",
        normalized_query="SELECT * FROM t",
        operator_tree=operator_tree_b,
        rls_policy_ids=["p1", "p2"],
        predicate_hash="pred456",
        planner_version="v1",
    )
    plan = build_plan_response(
        dialect="native",
        query_text="SELECT 1",
        operator_tree={"operator_id": "root", "operator_type": "Read", "children": []},
        rls_policy_ids=["policy_a"],
        predicate_hash="pred",
        planner_version="v1",
        rls_applied=True,
    )
    save(
        "07/plan_hash_report.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=[
                check_payload("plan_test_suite", plan_suite.passed, f"tests_run={plan_suite.tests_run}"),
                check_payload("hash_stable_for_equivalent_tree", plan_hash_a == plan_hash_b, f"{plan_hash_a} != {plan_hash_b}"),
                check_payload("plan_contains_required_fields", {"plan_hash", "operator_tree", "rls_visibility"}.issubset(plan.keys()), json.dumps(plan, sort_keys=True)),
            ],
            extra={
                "tests_run": plan_suite.tests_run,
                "duration_sec": round(plan_suite.duration_sec, 6),
                "plan_hash": plan_hash_a,
            },
        ),
    )
    save(
        "07/diff_report.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=[check_payload("plan_hash_changes_on_predicate_change", plan_hash_a != diff_hash, f"{plan_hash_a} == {diff_hash}")],
            extra={
                "base_hash": plan_hash_a,
                "changed_hash": diff_hash,
            },
        ),
    )

    mode_suite = run_suite(
        repo_root,
        [
            "test_execution_mode.py",
            "test_policy.py",
            "test_approval_store.py",
            "test_operational_controls.py",
        ],
    )
    simulation_rows = []
    for mode, statement_kind, approval_token, expected_allowed in [
        ("ai_analysis", "read", None, True),
        ("ai_analysis", "mutation", None, False),
        ("ai_mutation_pending_approval", "mutation", None, False),
        ("ai_mutation_approved", "mutation", "approved-token", True),
    ]:
        try:
            evaluation, _, normalized_options = evaluate_execution_mode(
                mode=mode,
                statement_kind=statement_kind,
                approval=ApprovalEvidence(approval_token=approval_token),
                options={"max_rows": 10},
            )
            simulation_rows.append(
                {
                    "mode": mode,
                    "statement_kind": statement_kind,
                    "allowed": evaluation.allowed,
                    "expected_allowed": expected_allowed,
                    "rule_id": evaluation.rule_id,
                    "error_code": evaluation.error_code,
                    "normalized_options": normalized_options,
                }
            )
        except Exception as exc:  # pragma: no cover - surfaced in report payload
            simulation_rows.append(
                {
                    "mode": mode,
                    "statement_kind": statement_kind,
                    "allowed": False,
                    "expected_allowed": expected_allowed,
                    "rule_id": "EXCEPTION",
                    "error_code": exc.__class__.__name__,
                    "normalized_options": {},
                }
            )
    mode_checks = [
        check_payload("execution_mode_suite", mode_suite.passed, f"tests_run={mode_suite.tests_run}"),
        check_payload(
            "policy_simulation_matches_expectations",
            all(row["allowed"] == row["expected_allowed"] for row in simulation_rows),
            json.dumps(simulation_rows, sort_keys=True),
        ),
        check_payload(
            "pending_to_approved_requires_approval",
            not validate_transition(
                from_mode="ai_mutation_pending_approval",
                to_mode="ai_mutation_approved",
                approval=ApprovalEvidence(approval_token=None),
            )
            and validate_transition(
                from_mode="ai_mutation_pending_approval",
                to_mode="ai_mutation_approved",
                approval=ApprovalEvidence(approval_token="approved-token"),
            ),
            "transition policy mismatch",
        ),
    ]
    save(
        "08/mode_matrix.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=mode_checks,
            extra={
                "tests_run": mode_suite.tests_run,
                "duration_sec": round(mode_suite.duration_sec, 6),
                "simulated_cases": len(simulation_rows),
            },
        ),
    )
    save(
        "08/policy_simulation.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=[check_payload("simulated_cases_recorded", len(simulation_rows) == 4, json.dumps(simulation_rows, sort_keys=True))],
            extra={"cases": simulation_rows},
        ),
    )

    audit_suite = run_suite(repo_root, ["test_audit_bundle.py"])
    bundle = create_audit_bundle(
        trace_id="tr_1",
        request_id="req_1",
        tenant_id="tenant_a",
        actor_id="actor_a",
        dialect="native",
        execution_mode="ai_analysis",
        sql_text_normalized="SELECT 1",
        compile_artifact_id="cmp_1",
        execution_artifact_id="exe_1",
        plan_json={"operator_tree": {"operator_id": "root", "operator_type": "Read", "children": []}},
        plan_hash="plan_1",
        policy_decision="allow",
        policy_rule_id="MODE-ALLOW-READ-001",
        security_context={
            "tenant_id": "tenant_a",
            "actor_id": "actor_a",
            "roles": [],
            "context_version": 1,
        },
        created_at_utc=generated_at,
        statement_kind="read",
        sblr_hash="hash123",
    )
    replay_ok = replay_validate_bundle(
        bundle=bundle,
        security_context={
            "tenant_id": "tenant_a",
            "actor_id": "actor_a",
            "roles": [],
            "context_version": 1,
        },
        expected_policy_decision="allow",
        expected_plan_hash="plan_1",
    )
    tampered = dict(bundle)
    tampered["policy_rule_id"] = "tampered"
    replay_bad = replay_validate_bundle(bundle=tampered)
    attestation_secret = (
        os.getenv("SCRATCHBIRD_AI_AUDIT_ATTESTATION_SECRET")
        or "scratchbird-ai-local-conformance-secret"
    )
    attestation = create_bundle_attestation(
        bundle=bundle,
        attestor_id="scratchbird-ai-conformance",
        attestation_mode="hmac_sha256",
        shared_secret=attestation_secret,
    )
    attestation_verified = verify_bundle_attestation(
        bundle=bundle,
        attestation=attestation,
        shared_secret=attestation_secret,
    )
    save(
        "09/audit_replay_report.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=[
                check_payload("audit_suite", audit_suite.passed, f"tests_run={audit_suite.tests_run}"),
                check_payload("replay_match_outcome", replay_ok.outcome == REPLAY_OUTCOMES["match"], replay_ok.reason),
                check_payload("tamper_detection_outcome", replay_bad.outcome == REPLAY_OUTCOMES["mismatch_hash"], replay_bad.reason),
            ],
            extra={
                "tests_run": audit_suite.tests_run,
                "duration_sec": round(audit_suite.duration_sec, 6),
                "bundle_hash": bundle["bundle_hash"],
            },
        ),
    )
    save(
        "09/attestation_report.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=[
                check_payload(
                    "bundle_hash_present",
                    bool(bundle.get("bundle_hash")),
                    json.dumps(bundle, sort_keys=True),
                ),
                check_payload(
                    "attestation_verified",
                    attestation_verified.outcome == ATTESTATION_OUTCOMES["verified"],
                    attestation_verified.reason,
                ),
            ],
            extra={
                "attestation_style": "hmac_sha256",
                "bundle_hash": bundle["bundle_hash"],
                "attestation": attestation,
                "attestation_verification_outcome": attestation_verified.outcome,
            },
        ),
    )

    cluster_suite = run_suite(repo_root, ["test_cluster_routing.py"])
    topology = ClusterTopology(
        cluster_epoch=5,
        shards=(
            ClusterShard(
                shard_id="s1",
                tenant_ids=("tenant_a",),
                status="healthy",
                primary=ClusterNode(node_id="n1", health="healthy"),
                replicas=(ClusterNode(node_id="n2", health="healthy"),),
            ),
            ClusterShard(
                shard_id="s2",
                tenant_ids=("tenant_a",),
                status="healthy",
                primary=ClusterNode(node_id="n3", health="offline"),
                replicas=(ClusterNode(node_id="n4", health="healthy"),),
            ),
        ),
    )
    route = route_query(topology=topology, tenant_id="tenant_a")
    distributed = distributed_vector_search(
        topology=topology,
        tenant_id="tenant_a",
        per_shard_results={
            "s1": [{"document_id": "doc1", "score": 0.7}],
            "s2": [{"document_id": "doc2", "score": 0.8}],
            "s999": [{"document_id": "doc_x", "score": 1.0}],
        },
        top_k=5,
    )
    save(
        "10/routing_report.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=[
                check_payload("cluster_suite", cluster_suite.passed, f"tests_run={cluster_suite.tests_run}"),
                check_payload("primary_and_failover_routes_selected", {(item.shard_id, item.node_id) for item in route.routes} == {("s1", "n1"), ("s2", "n4")}, json.dumps([(item.shard_id, item.node_id) for item in route.routes])),
                check_payload("distributed_results_filter_unknown_shards", [row["document_id"] for row in distributed["results"]] == ["doc2", "doc1"], json.dumps(distributed, sort_keys=True)),
            ],
            extra={
                "tests_run": cluster_suite.tests_run,
                "duration_sec": round(cluster_suite.duration_sec, 6),
                "route_count": len(route.routes),
            },
        ),
    )
    save(
        "10/failover_report.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=[check_payload("replica_failover_reason_recorded", any(item.route_reason == "replica_failover" for item in route.routes), json.dumps([item.route_reason for item in route.routes]))],
            extra={
                "routes": [
                    {"shard_id": item.shard_id, "node_id": item.node_id, "route_reason": item.route_reason}
                    for item in route.routes
                ],
            },
        ),
    )

    framework_suite = run_suite(repo_root, ["test_framework_adapters.py"])
    save_junit("12/test_report.junit.xml", "framework_adapter_parity", framework_suite)
    ctx = security_context()

    canonical_langchain_service = build_default_service()
    langchain_service = build_default_service()
    langchain_adapter = LangChainAdapter(langchain_service)
    canonical_langchain_query = canonical_langchain_service.execute_readonly_query(
        request_id="req_framework_langchain_query",
        dialect="native",
        query_text="SELECT 1",
        security_context=ctx,
        options={"max_rows": 1},
    )
    langchain_query = langchain_adapter.run_query(
        dialect="native",
        query_text="SELECT 1",
        security_context=ctx,
        options={"max_rows": 1},
        request_id="req_framework_langchain_query",
    )

    canonical_llama_service = build_default_service()
    llama_service = build_default_service()
    seed_framework_service(canonical_llama_service)
    seed_framework_service(llama_service)
    llama_adapter = LlamaIndexAdapter(llama_service)
    canonical_llama_explain = canonical_llama_service.explain_query(
        dialect="native",
        query_text="SELECT 1",
        context={"security_context": ctx},
    )
    llama_explain = llama_adapter.explain(
        dialect="native",
        query_text="SELECT 1",
        security_context=ctx,
    )
    canonical_llama_vector = canonical_llama_service.vector_search(
        index_id="idx_framework_docs",
        query_embedding=[0.1, 0.2, 0.3],
        top_k=5,
        security_context=ctx,
    )
    llama_vector = llama_adapter.vector_retrieve(
        index_id="idx_framework_docs",
        query_embedding=[0.1, 0.2, 0.3],
        top_k=5,
        security_context=ctx,
    )
    canonical_llama_hybrid = canonical_llama_service.hybrid_search(
        dialect="native",
        query_text="north overdue invoice",
        query_embedding=[0.1, 0.2, 0.3],
        vector_index_id="idx_framework_docs",
        top_k=5,
        security_context=ctx,
        sql_filter={"metadata": {"document_id": "doc-1"}},
    )
    llama_hybrid = llama_adapter.hybrid_retrieve(
        dialect="native",
        query_text="north overdue invoice",
        query_embedding=[0.1, 0.2, 0.3],
        vector_index_id="idx_framework_docs",
        top_k=5,
        security_context=ctx,
        sql_filter={"metadata": {"document_id": "doc-1"}},
    )

    canonical_semantic_service = build_default_service()
    semantic_service = build_default_service()
    semantic_adapter = SemanticKernelAdapter(semantic_service)
    canonical_semantic_query = canonical_semantic_service.execute_readonly_query(
        request_id="req_framework_semantic_query",
        dialect="native",
        query_text="SELECT 1",
        security_context=ctx,
        options={"max_rows": 1},
    )
    semantic_query = semantic_adapter.invoke_function(
        function_name="execute_readonly_query",
        arguments={
            "dialect": "native",
            "query_text": "SELECT 1",
            "options": {"max_rows": 1},
        },
        security_context=ctx,
        request_id="req_framework_semantic_query",
    )
    framework_checks = [
        check_payload("framework_adapter_suite", framework_suite.passed, f"tests_run={framework_suite.tests_run}"),
        check_payload(
            "langchain_query_parity",
            langchain_query["status"] == "success" and langchain_query["result"] == canonical_langchain_query,
            json.dumps(
                {
                    "adapter": langchain_query,
                    "canonical": canonical_langchain_query,
                },
                sort_keys=True,
            ),
        ),
        check_payload(
            "llamaindex_explain_parity",
            llama_explain["status"] == "success" and llama_explain["result"] == canonical_llama_explain,
            json.dumps(
                {
                    "adapter": llama_explain,
                    "canonical": canonical_llama_explain,
                },
                sort_keys=True,
            ),
        ),
        check_payload(
            "llamaindex_vector_parity",
            llama_vector["status"] == "success" and llama_vector["result"] == canonical_llama_vector,
            json.dumps(
                {
                    "adapter": llama_vector,
                    "canonical": canonical_llama_vector,
                },
                sort_keys=True,
            ),
        ),
        check_payload(
            "llamaindex_hybrid_parity",
            llama_hybrid["status"] == "success" and llama_hybrid["result"] == canonical_llama_hybrid,
            json.dumps(
                {
                    "adapter": llama_hybrid,
                    "canonical": canonical_llama_hybrid,
                },
                sort_keys=True,
            ),
        ),
        check_payload(
            "semantic_kernel_query_parity",
            semantic_query["status"] == "success" and semantic_query["result"] == canonical_semantic_query,
            json.dumps(
                {
                    "adapter": semantic_query,
                    "canonical": canonical_semantic_query,
                },
                sort_keys=True,
            ),
        ),
    ]
    save(
        "12/framework_parity.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=framework_checks,
            extra={
                "tests_run": framework_suite.tests_run,
                "duration_sec": round(framework_suite.duration_sec, 6),
                "profiles": ["langchain_v0", "llamaindex_v0", "semantic_kernel_v0"],
            },
        ),
    )

    provider_suite = run_suite(repo_root, ["test_provider_profiles.py"])
    save_junit("13/test_report.junit.xml", "provider_profile_parity", provider_suite)
    provider_catalog_service = build_default_service()
    provider_catalog = provider_catalog_service.get_provider_profiles()

    def canonical_provider_envelope(call_id: str, request_id: str) -> dict[str, Any]:
        service = build_default_service()
        return service.invoke_tool(
            payload={
                "request_id": request_id,
                "call_id": call_id,
                "tool_name": "execute_readonly_query",
                "arguments": {
                    "dialect": "native",
                    "query_text": "SELECT 1",
                    "security_context": {
                        "tenant_id": "tenant_a",
                        "actor_id": "actor_a",
                    },
                    "options": {"max_rows": 1},
                },
            },
            interface_profile_id="service_internal_v0",
        )

    canonical_openai = canonical_provider_envelope("call_provider_openai", "req_provider_openai")
    openai_provider = build_default_service().invoke_provider_tool(
        provider_profile_id="openai_tool_calling_v0",
        payload={
            "request_id": "req_provider_openai",
            "id": "call_provider_openai",
            "function": {
                "name": "execute_readonly_query",
                "arguments": (
                    '{"dialect":"native","query_text":"SELECT 1","security_context":'
                    '{"tenant_id":"tenant_a","actor_id":"actor_a"},"options":{"max_rows":1}}'
                ),
            },
        },
    )
    canonical_anthropic = canonical_provider_envelope(
        "call_provider_anthropic",
        "req_provider_anthropic",
    )
    anthropic_provider = build_default_service().invoke_provider_tool(
        provider_profile_id="anthropic_tool_use_v0",
        payload={
            "request_id": "req_provider_anthropic",
            "id": "call_provider_anthropic",
            "name": "execute_readonly_query",
            "input": {
                "dialect": "native",
                "query_text": "SELECT 1",
                "security_context": {"tenant_id": "tenant_a", "actor_id": "actor_a"},
                "options": {"max_rows": 1},
            },
        },
    )
    canonical_gemini = canonical_provider_envelope(
        "call_provider_gemini",
        "req_provider_gemini",
    )
    gemini_provider = build_default_service().invoke_provider_tool(
        provider_profile_id="gemini_function_calling_v0",
        payload={
            "request_id": "req_provider_gemini",
            "functionCall": {
                "id": "call_provider_gemini",
                "name": "execute_readonly_query",
                "args": {
                    "dialect": "native",
                    "query_text": "SELECT 1",
                    "security_context": {"tenant_id": "tenant_a", "actor_id": "actor_a"},
                    "options": {"max_rows": 1},
                },
            },
        },
    )
    unknown_provider = build_default_service().invoke_provider_tool(
        provider_profile_id="unknown_profile_v0",
        payload={
            "request_id": "req_provider_unknown",
            "id": "call_provider_unknown",
            "name": "execute_readonly_query",
            "input": {
                "dialect": "native",
                "query_text": "SELECT 1",
                "security_context": {"tenant_id": "tenant_a", "actor_id": "actor_a"},
            },
        },
    )
    provider_checks = [
        check_payload("provider_profile_suite", provider_suite.passed, f"tests_run={provider_suite.tests_run}"),
        check_payload(
            "provider_catalog_implemented",
            {
                item["profile_id"]: item["state"] for item in provider_catalog["profiles"]
            }
            == {
                "openai_tool_calling_v0": "implemented",
                "anthropic_tool_use_v0": "implemented",
                "gemini_function_calling_v0": "implemented",
            },
            json.dumps(provider_catalog, sort_keys=True),
        ),
        check_payload(
            "openai_provider_parity",
            openai_provider["status"] == canonical_openai["status"]
            and openai_provider["result"] == canonical_openai["result"]
            and openai_provider["structured_output"] == canonical_openai["structured_output"]
            and openai_provider["trace_id"] == canonical_openai["trace_id"],
            json.dumps(
                {
                    "provider": openai_provider,
                    "canonical": canonical_openai,
                },
                sort_keys=True,
            ),
        ),
        check_payload(
            "anthropic_provider_parity",
            anthropic_provider["status"] == canonical_anthropic["status"]
            and anthropic_provider["result"] == canonical_anthropic["result"]
            and anthropic_provider["structured_output"] == canonical_anthropic["structured_output"]
            and anthropic_provider["trace_id"] == canonical_anthropic["trace_id"],
            json.dumps(
                {
                    "provider": anthropic_provider,
                    "canonical": canonical_anthropic,
                },
                sort_keys=True,
            ),
        ),
        check_payload(
            "gemini_provider_parity",
            gemini_provider["status"] == canonical_gemini["status"]
            and gemini_provider["result"] == canonical_gemini["result"]
            and gemini_provider["structured_output"] == canonical_gemini["structured_output"]
            and gemini_provider["trace_id"] == canonical_gemini["trace_id"],
            json.dumps(
                {
                    "provider": gemini_provider,
                    "canonical": canonical_gemini,
                },
                sort_keys=True,
            ),
        ),
        check_payload(
            "unknown_provider_rejected",
            unknown_provider["status"] == "error"
            and unknown_provider["error"]["error_code"] == "E_PROVIDER_CONTRACT_UNSUPPORTED",
            json.dumps(unknown_provider, sort_keys=True),
        ),
    ]
    save(
        "13/provider_parity.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=provider_checks,
            extra={
                "tests_run": provider_suite.tests_run,
                "duration_sec": round(provider_suite.duration_sec, 6),
                "profiles": [
                    "openai_tool_calling_v0",
                    "anthropic_tool_use_v0",
                    "gemini_function_calling_v0",
                ],
            },
        ),
    )

    release_suite = run_suite(repo_root, ["test_validate_evidence_gates.py"])
    certification_service = build_default_service()
    certification_manifest = dict(certification_service.export_certification_manifest())
    certification_manifest["git_commit"] = git_sha
    save(
        "11/environment_manifest.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=[
                check_payload(
                    "certification_manifest_git_commit_matches",
                    certification_manifest["git_commit"] == git_sha,
                    certification_manifest["git_commit"],
                ),
                check_payload(
                    "environment_descriptor_present",
                    bool(certification_manifest.get("environment_descriptor")),
                    json.dumps(certification_manifest, sort_keys=True)[:1000],
                ),
                check_payload(
                    "compatibility_manifest_embedded",
                    bool(certification_manifest.get("compatibility_manifest")),
                    json.dumps(certification_manifest, sort_keys=True)[:1000],
                ),
            ],
            extra={
                "certification_manifest": certification_manifest,
            },
        ),
    )
    release_checks = [
        check_payload("validator_suite", release_suite.passed, f"tests_run={release_suite.tests_run}"),
        check_payload(*check_line_contains(repo_root / "README.md", "docs/status/EARLY_BETA_STATUS_2026-03-07.md")),
        check_payload(*check_line_contains(repo_root / "docs" / "README.md", "status/EARLY_BETA_STATUS_2026-03-07.md")),
        check_payload(
            "artifact_output_root_created",
            artifacts_root.exists(),
            str(artifacts_root),
        ),
        check_payload(
            "release_spec_declares_conformance_artifacts",
            "artifacts/ai_conformance/11/matrix_status.json"
            in (repo_root / "docs" / "releases" / "EARLY_BETA_CONFORMANCE_GATES.md").read_text(
                encoding="utf-8"
            ),
            "docs/releases/EARLY_BETA_CONFORMANCE_GATES.md",
        ),
    ]
    save(
        "11/matrix_status.json",
        artifact_payload(
            generated_at_utc=generated_at,
            git_sha=git_sha,
            checks=release_checks,
            extra={
                "tests_run": release_suite.tests_run,
                "duration_sec": round(release_suite.duration_sec, 6),
                "artifact_count": len(generated_files),
            },
        ),
    )

    validate_ok, validate_output = run_command(
        repo_root,
        [
            sys.executable,
            "tools/validate_evidence_gates.py",
            "--repo-root",
            ".",
            "--spec",
            "docs/releases/EARLY_BETA_CONFORMANCE_GATES.md",
            "--artifact-root",
            str(artifacts_base),
        ],
        runtime_root=runtime_root,
    )
    matrix_path = artifacts_root / "11" / "matrix_status.json"
    matrix_payload = json.loads(matrix_path.read_text(encoding="utf-8"))
    validator_check = check_payload("release_gate_validator", validate_ok, validate_output)
    checks = [("validator_suite", release_suite.passed, f"tests_run={release_suite.tests_run}"), validator_check]
    checks.extend(release_checks[1:])
    matrix_payload = artifact_payload(
        generated_at_utc=generated_at,
        git_sha=git_sha,
        checks=checks,
        extra={
            "tests_run": release_suite.tests_run,
            "duration_sec": round(release_suite.duration_sec, 6),
            "artifact_count": len(generated_files),
            "validator_output": validate_output,
        },
    )
    write_json(matrix_path, matrix_payload)

    if not validate_ok:
        print(validate_output, file=sys.stderr)
        return 1

    print(
        "Generated AI conformance artifacts "
        f"(git_commit={git_sha}, generated_at_utc={generated_at}, files={len(generated_files)})"
    )
    return 0


def main() -> None:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    artifact_root = Path(args.artifact_root).resolve() if args.artifact_root else None
    raise SystemExit(generate(repo_root, artifact_root=artifact_root))


if __name__ == "__main__":
    main()
