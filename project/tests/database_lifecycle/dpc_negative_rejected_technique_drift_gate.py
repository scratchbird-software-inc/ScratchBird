#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DPC-067 negative rejected-technique drift gate.

The gate intentionally does not read execution_plan artifacts. It scans project
source/test/CMake surfaces for rejected implementation patterns and runs
detector self-checks so the CTest is not a documentation-only placeholder.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys


DPC_NEGATIVE_DRIFT = "DPC_NEGATIVE_DRIFT"
SELF = Path("project/tests/database_lifecycle/dpc_negative_rejected_technique_drift_gate.py")

SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".csv",
    ".dart",
    ".go",
    ".h",
    ".hh",
    ".hpp",
    ".inc",
    ".java",
    ".js",
    ".json",
    ".kt",
    ".md",
    ".php",
    ".py",
    ".rb",
    ".rs",
    ".sh",
    ".sql",
    ".swift",
    ".ts",
    ".cmake",
    ".txt",
    ".yaml",
    ".yml",
}

SCAN_ROOTS = (
    Path("project/CMakeLists.txt"),
    Path("project/cmake"),
    Path("project/drivers"),
    Path("project/src"),
    Path("project/tests/agents"),
    Path("project/tests/database_lifecycle"),
    Path("project/tests/sbsql_parser_worker"),
    Path("project/tools"),
)

EXCLUDED_PARTS = {
    "." "git",
    ".dart_tool",
    ".pytest_cache",
    "__pycache__",
    "build",
    "node_modules",
    "target",
    "vendor",
}

EXCLUDED_PATH_PREFIXES = (
    "project/drivers/fixtures/",
    "project/tests/database_lifecycle/fixtures/",
    "project/tests/sbsql_parser_worker/fixtures/",
    "project/tests/sbsql_parser_worker/generated/",
)

ALLOWED_UUID_LITERAL_PREFIXES = (
    "project/src/engine/functions/registry/",
    "project/src/engine/functions/generated/",
    "project/src/parsers/sbsql_worker/registry/generated/",
)

ALLOWED_UUID_LITERAL_FILES = {
    Path("project/src/core/index/page_extent_summary.cpp"),
    Path("project/src/engine/sblr/sblr_context_variables.cpp"),
    Path("project/src/engine/sblr/sblr_operator_runtime_06_json_collection_range.inc"),
    Path("project/src/parsers/native/v3/package/native_v3_parser_package.cpp"),
    Path("project/src/parsers/sbsql_worker/meta/meta_command_surface.cpp"),
    Path("project/src/parsers/sbsql_worker/rendering/rendering.cpp"),
    Path("project/src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp"),
    Path("project/src/server/ipc_tester.cpp"),
    Path("project/src/server/sblr_dispatch_server.cpp"),
    Path("project/src/udr/sbu_firebird_parser_support/sbu_firebird_parser_support.hpp"),
    Path("project/src/udr/sbu_sbsql_parser_support/sbu_sbsql_parser_support.hpp"),
}

ALLOWED_CLUSTER_BOUNDARY_PREFIXES = (
    "project/src/cluster/",
    "project/src/cluster_provider/",
    "project/src/cluster_provider_stub/",
    "project/src/engine/internal_api/cluster/",
)

ALLOWED_CLUSTER_BOUNDARY_FILES = {
    Path("project/CMakeLists.txt"),
    Path("project/src/core/agents/agent_cluster_boundary.cpp"),
    Path("project/src/core/agents/agent_cluster_boundary.hpp"),
    Path("project/src/core/agents/agents/cluster_autoscale_manager.cpp"),
    Path("project/src/core/agents/agents/cluster_scheduler_manager.cpp"),
    Path("project/src/core/agents/agents/cluster_upgrade_manager.cpp"),
    Path("project/src/engine/optimizer/cluster_candidate.cpp"),
    Path("project/src/engine/optimizer/cluster_candidate.hpp"),
    Path("project/src/engine/optimizer/cluster_refusal_path.cpp"),
    Path("project/src/storage/page/cluster_transaction_page.cpp"),
    Path("project/src/storage/page/cluster_transaction_page.hpp"),
    Path("project/src/transaction/mga/cluster_transaction_fail_closed.cpp"),
    Path("project/src/transaction/mga/cluster_transaction_fail_closed.hpp"),
}

AUTHORITY_ACTORS_RE = re.compile(
    r"\b(parser|client|driver|donor|timestamp|uuid|uuidv[0-9]+|event[-_ ]?stream)\b",
    re.I,
)
AUTHORITY_ACTOR = r"(?:parser|client|driver|donor|timestamp|uuid|uuidv[0-9]+|event[-_ ]?stream)"
AUTHORITY_TARGET = (
    r"(?:commit|committed|rollback|visibility|visible|cleanup|recovery|recover|"
    r"finality|durable|durability|source[-_ ]?of[-_ ]?truth|authority|authoritative)"
)
CRITICAL_AUTHORITY_TARGET = (
    r"(?:commit|committed|rollback|visibility|visible|cleanup|recovery|recover|"
    r"finality|source[-_ ]?of[-_ ]?truth)"
)
AUTHORITY_TARGET_RE = re.compile(
    r"\b(commit|committed|rollback|visibility|visible|cleanup|recovery|recover|"
    r"finality|durable|durability|source[-_ ]of[-_ ]truth|authority|authoritative)\b",
    re.I,
)
AUTHORITY_POSITIVE = (
    r"(?:owns?|decides?|controls?|authorizes?|publishes?|determines?|drives?|"
    r"requires?|required|source[-_ ]?of[-_ ]?truth|authoritative|finality)"
)
AUTHORITY_POSITIVE_RE = re.compile(r"\b" + AUTHORITY_POSITIVE + r"\b", re.I)
EXTERNAL_AUTHORITY_DRIFT_RE = re.compile(
    r"\b" + AUTHORITY_ACTOR + r"\b.{0,80}\b" + AUTHORITY_POSITIVE
    + r"\b.{0,80}\b" + AUTHORITY_TARGET + r"\b|"
    r"\b" + AUTHORITY_ACTOR + r"\b.{0,80}\b" + CRITICAL_AUTHORITY_TARGET
    + r"\b.{0,80}\b" + AUTHORITY_POSITIVE + r"\b|"
    r"\b" + CRITICAL_AUTHORITY_TARGET + r"\b.{0,80}\b(?:is|=|:|from|by|owned by|driven by|uses?)"
    + r"\b.{0,80}\b" + AUTHORITY_ACTOR + r"\b|"
    r"\bauthority[_ -]?(?:source|owner)\s*[:=]\s*[\"']?" + AUTHORITY_ACTOR + r"\b",
    re.I,
)
WAL_RE = re.compile(r"\b(WAL|write[-_ ]?ahead|redo|journal)\b", re.I)
WAL_TARGET_RE = re.compile(
    r"\b(transaction|commit|committed|rollback|visibility|visible|recovery|"
    r"recover|finality|durable|durability|source[-_ ]of[-_ ]truth|authority|"
    r"authoritative)\b",
    re.I,
)
WAL_DIRECT_AUTHORITY_TARGET_RE = re.compile(
    r"\b(recovery|recover|finality|source[-_ ]of[-_ ]truth|authority|"
    r"authoritative)\b",
    re.I,
)
UUID_LITERAL_RE = re.compile(
    r"\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
    r"[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\b"
)
CATALOG_OBJECT_RE = re.compile(
    r"\b(catalog|schema|table|relation|index|object|descriptor|function|"
    r"operator|package|policy|principal|role|filespace|database)_?uuid\b|"
    r"\bresolved_object_uuids\b|\bobject identity\b",
    re.I,
)
CLUSTER_PRIVATE_RE = re.compile(
    r"\b(cluster_private|private_cluster|private[-_ ]cluster|"
    r"private_cluster_execution\s*=\s*true|closed[-_ ]source[-_ ]cluster)\b",
    re.I,
)
CLUSTER_IMPL_RE = re.compile(
    r"\b(implementation|executor|dispatch|enabled|active|executes?|runs?|"
    r"private_cluster_execution\s*=\s*true)\b",
    re.I,
)
DRIVER_LANGUAGE_RE = re.compile(
    r"\b(driver|python|java|jdbc|odbc|node|nodejs|rust|go|swift|php|ruby|"
    r"dotnet|csharp|dart|elixir|perl|r2dbc|adbc|flight_sql|language)\b",
    re.I,
)
SPEED_CLAIM_RE = re.compile(
    r"\b(faster|fastest|speedup|speed[-_ ]claim|throughput|latency|ops/sec|"
    r"qps|benchmark|performance[-_ ]evidence|database[-_ ]benchmark)\b",
    re.I,
)
BENCHMARK_EVIDENCE_RE = re.compile(
    r"\b(database[-_ ]benchmark|benchmark[-_ ]evidence|speed[-_ ]claim|"
    r"performance[-_ ]claim|proof|evidence)\b",
    re.I,
)
UNBOUNDED_LOOP_RE = re.compile(
    r"\bwhile\s*\(\s*true\s*\)|\bfor\s*\(\s*;\s*;\s*\)|\.detach\s*\(",
    re.I,
)
AGENT_CONTEXT_RE = re.compile(r"\b(agent|background|worker|scheduler|maintenance)\b", re.I)
BOUNDING_RE = re.compile(
    r"\b(budget|bounded|quota|resource[_ -]?governor|backpressure|limit|"
    r"deadline|shutdown|stop|cancel|drain|observable|metric|capacity|throttle)\b",
    re.I,
)
NEGATIVE_RE = re.compile(
    r"\b(must not|must_not|never|forbid|forbidden|refus|reject|not authority|"
    r"not authoritative|no[_ -]?wal|anti[-_ ]?wal|without wal|does not|"
    r"non[-_ ]authority|fail[-_ ]closed|unavailable|false|"
    r"not parser|not client|not driver|not donor|no parser|no donor|"
    r"no wal|no write[-_ ]ahead|do not|cannot|without|invalid|diagnostic[-_ ]?only)\b|"
    r"\b(?:not|no|without|does not|do not|cannot)\b.{0,100}\b(authority|finality|execution|benchmark evidence)\b",
    re.I,
)


@dataclass(frozen=True)
class Finding:
    path: Path
    line_no: int
    category: str
    detail: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--build-root", required=True, type=Path)
    return parser.parse_args()


def rel_text(path: Path) -> str:
    return path.as_posix()


def is_excluded(rel_path: Path) -> bool:
    rel = rel_text(rel_path)
    if any(part in EXCLUDED_PARTS for part in rel_path.parts):
        return True
    return any(rel.startswith(prefix) for prefix in EXCLUDED_PATH_PREFIXES)


def iter_scan_files(repo_root: Path) -> list[Path]:
    files: list[Path] = []
    for root in SCAN_ROOTS:
        absolute = repo_root / root
        if absolute.is_file():
            if not is_excluded(root) and root.suffix in SOURCE_SUFFIXES:
                files.append(root)
            continue
        if not absolute.exists():
            continue
        for candidate in absolute.rglob("*"):
            if not candidate.is_file():
                continue
            rel_path = candidate.relative_to(repo_root)
            if is_excluded(rel_path):
                continue
            if rel_path.suffix not in SOURCE_SUFFIXES:
                continue
            files.append(rel_path)
    return sorted(set(files))


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def nearby_window(lines: list[str], index: int, radius: int = 1) -> str:
    start = max(0, index - radius)
    end = min(len(lines), index + radius + 1)
    return " ".join(lines[start:end])


def negative_context(text: str) -> bool:
    return bool(NEGATIVE_RE.search(text))


def allowed_uuid_literal_path(rel_path: Path) -> bool:
    rel = rel_text(rel_path)
    return rel_path in ALLOWED_UUID_LITERAL_FILES or any(
        rel.startswith(prefix) for prefix in ALLOWED_UUID_LITERAL_PREFIXES
    )


def allowed_cluster_boundary_path(rel_path: Path) -> bool:
    rel = rel_text(rel_path)
    return rel_path in ALLOWED_CLUSTER_BOUNDARY_FILES or any(
        rel.startswith(prefix) for prefix in ALLOWED_CLUSTER_BOUNDARY_PREFIXES
    )


def scans_authority_categories(rel_path: Path) -> bool:
    rel = rel_text(rel_path)
    return rel.startswith("project/src/")


def scans_driver_claims(rel_path: Path) -> bool:
    rel = rel_text(rel_path)
    return rel.startswith("project/src/") or rel.startswith("project/drivers/")


def scans_cluster_private(rel_path: Path) -> bool:
    rel = rel_text(rel_path)
    return rel.startswith("project/src/") or rel == "project/CMakeLists.txt"


def scans_uuid_literals(rel_path: Path) -> bool:
    return rel_text(rel_path).startswith("project/src/")


def scan_text(rel_path: Path, text: str) -> list[Finding]:
    findings: list[Finding] = []
    lines = text.splitlines()
    for index, line in enumerate(lines):
        line_no = index + 1
        window = nearby_window(lines, index)

        if (
            scans_authority_categories(rel_path)
            and WAL_RE.search(line)
            and (
                WAL_DIRECT_AUTHORITY_TARGET_RE.search(line)
                or (
                    WAL_TARGET_RE.search(line)
                    and (
                        AUTHORITY_POSITIVE_RE.search(line)
                        or re.search(r"\bauthority[_ -]?(?:source|owner)\b", line, re.I)
                    )
                )
            )
        ):
            if not negative_context(window):
                findings.append(
                    Finding(rel_path, line_no, "wal_recovery_authority",
                            "WAL/write-ahead/redo/journal authority drift")
                )

        if (
            scans_authority_categories(rel_path)
            and EXTERNAL_AUTHORITY_DRIFT_RE.search(line)
            and not negative_context(nearby_window(lines, index, radius=3))
        ):
            if "uuid" in line.lower() and "api owns" in line.lower():
                continue
            findings.append(
                Finding(rel_path, line_no, "external_finality_authority",
                        "parser/client/driver/donor/timestamp/UUID/event-stream authority drift")
            )

        if (
            scans_uuid_literals(rel_path)
            and UUID_LITERAL_RE.search(line)
            and CATALOG_OBJECT_RE.search(window)
        ):
            if not allowed_uuid_literal_path(rel_path):
                findings.append(
                    Finding(rel_path, line_no, "hard_coded_catalog_object_uuid",
                            "hard-coded catalog/object UUID literal outside allowed system domains")
                )

        if (
            scans_cluster_private(rel_path)
            and CLUSTER_PRIVATE_RE.search(line)
            and CLUSTER_IMPL_RE.search(line)
        ):
            if not allowed_cluster_boundary_path(rel_path) and not negative_context(window):
                findings.append(
                    Finding(rel_path, line_no, "cluster_private_core_code",
                            "cluster-private implementation marker outside provider/stub boundary")
                )

        if (
            scans_driver_claims(rel_path)
            and DRIVER_LANGUAGE_RE.search(line)
            and SPEED_CLAIM_RE.search(line)
            and BENCHMARK_EVIDENCE_RE.search(line)
            and not negative_context(line)
        ):
            findings.append(
                Finding(rel_path, line_no, "driver_speed_benchmark_claim",
                        "driver-language speed claim used as database benchmark evidence")
            )

        if UNBOUNDED_LOOP_RE.search(line) and AGENT_CONTEXT_RE.search(window):
            bounded_window = nearby_window(lines, index, radius=3)
            if not BOUNDING_RE.search(bounded_window):
                findings.append(
                    Finding(rel_path, line_no, "unbounded_background_agent",
                            "background agent loop lacks budget/resource/backpressure evidence")
                )
    return findings


def synthetic_uuid_literal() -> str:
    return "-".join(("aaaaaaaa", "bbbb", "7ccc", "8ddd", "eeeeeeeeeeee"))


def detector_self_check() -> list[str]:
    errors: list[str] = []
    samples = (
        ("wal_recovery_authority", "commit recovery source of truth is WAL"),
        ("wal_recovery_authority", "WAL is required recovery authority"),
        ("wal_recovery_authority", "redo finality authority is diagnostic truth"),
        ("external_finality_authority", "parser owns commit finality for cleanup"),
        ("external_finality_authority", "donor visibility authority is required"),
        (
            "hard_coded_catalog_object_uuid",
            f'catalog object_uuid = "{synthetic_uuid_literal()}"',
        ),
        ("cluster_private_core_code", "cluster_private executor implementation enabled"),
        (
            "driver_speed_benchmark_claim",
            "python driver speedup is database benchmark evidence",
        ),
        ("unbounded_background_agent", "background agent while (true) do_work();"),
    )
    expected = {category for category, _sample in samples}
    observed: set[str] = set()
    for category, sample in samples:
        path = Path("project/src/core/dpc067_sentinel.cpp")
        sample_categories = {finding.category for finding in scan_text(path, sample)}
        if category not in sample_categories:
            errors.append(
                f"detector self-check missed {category!r} for sample {sample!r}; "
                f"observed {sorted(sample_categories)}"
            )
        observed.update(sample_categories)
    missing = expected - observed
    if missing:
        errors.append(f"detector self-check missing categories: {sorted(missing)}")

    allowed_samples = {
        "wal_negative": "WAL is not recovery authority and must not decide commit",
        "actor_negative": "parser finality authority is forbidden",
        "uuid_test_path": f'catalog object_uuid = "{synthetic_uuid_literal()}"',
        "cluster_boundary": "cluster_private route fails closed at provider boundary",
        "driver_negative": "driver language speed is not benchmark evidence",
        "bounded_agent": "background agent while (true) obeys budget and backpressure",
    }
    allowed_findings = []
    allowed_findings.extend(scan_text(Path("project/src/core/dpc067_allowed.cpp"),
                                      allowed_samples["wal_negative"]))
    allowed_findings.extend(scan_text(Path("project/src/core/dpc067_allowed.cpp"),
                                      allowed_samples["actor_negative"]))
    allowed_findings.extend(
        scan_text(Path("project/src/engine/functions/registry/dpc067_allowed.cpp"),
                  allowed_samples["uuid_test_path"])
    )
    allowed_findings.extend(
        scan_text(Path("project/src/cluster_provider/dpc067_allowed.cpp"),
                  allowed_samples["cluster_boundary"])
    )
    allowed_findings.extend(scan_text(Path("project/src/core/dpc067_allowed.cpp"),
                                      allowed_samples["driver_negative"]))
    allowed_findings.extend(scan_text(Path("project/src/core/dpc067_allowed.cpp"),
                                      allowed_samples["bounded_agent"]))
    if allowed_findings:
        errors.append(
            "detector self-check false positives: "
            + "; ".join(f"{f.category}:{f.detail}" for f in allowed_findings)
        )
    return errors


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    build_root = args.build_root.resolve()
    errors = detector_self_check()

    if "docs" "/execution-plans" in rel_text(repo_root) or "docs" "/execution-plans" in rel_text(build_root):
        errors.append("gate roots must not point into private execution_plan directories")

    scan_files = iter_scan_files(repo_root)
    findings: list[Finding] = []
    for rel_path in scan_files:
        if rel_path == SELF:
            continue
        try:
            findings.extend(scan_text(rel_path, read_text(repo_root / rel_path)))
        except OSError as exc:
            errors.append(f"{rel_path}: failed to read: {exc}")

    if findings:
        for finding in findings[:200]:
            print(
                f"ERROR: {finding.path}:{finding.line_no}: "
                f"{finding.category}: {finding.detail}",
                file=sys.stderr,
            )
        if len(findings) > 200:
            print(f"ERROR: {len(findings) - 200} additional findings omitted", file=sys.stderr)
        return 1

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(
        f"{DPC_NEGATIVE_DRIFT}=passed: "
        f"{len(scan_files)} project source/test/CMake files scanned; "
        "rejected technique detectors active."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
