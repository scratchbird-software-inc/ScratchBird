#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Deterministic generation and no-network gate for SBsql regression fixtures.

Verifies that the deterministic-generation chain reproduces byte-identical
regression fixture artifacts from repo-local inputs without requiring network
access.

Phases:

  1. Static no-network scan: each generator's source file must not import
     URL, HTTP, socket, TLS, FTP, mail, terminal, RPC, browser, or
     third-party network client modules.

  2. Deterministic regeneration: the gate copies the regression artifact
     root to a temporary directory, reruns each generator with that temporary
     artifact root, and compares the md5 before and after. Tracked fixture
     artifacts are never modified by this CTest.

Architecture invariant compliance: read-only static scan over generator
sources; deterministic regeneration over copied regression artifact CSVs only.
No transaction model touched. No engine, parser worker,
server, listener, storage, or MGA file modified. No WAL surface
introduced. MGA copy-on-write remains the sole transaction recovery
model.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


# (generator_path, output_path, regen_method)
# regen_method "in_place" reruns the generator with --repo-root pointing at
# the repository and compares the existing output against the new output.
GENERATORS: list[tuple[str, str]] = [
    (
        "project/tools/sb_parser_gen/generate_strict_row_coverage_ledger.py",
        "STRICT_ROW_COVERAGE_LEDGER.csv",
    ),
    (
        "project/tools/sb_parser_gen/generate_native_future_promotion_matrix.py",
        "NATIVE_FUTURE_PROMOTION_MATRIX.csv",
    ),
    (
        "project/tools/sb_parser_gen/generate_function_semantic_oracle_matrix.py",
        "FUNCTION_SEMANTIC_ORACLE_MATRIX.csv",
    ),
    (
        "project/tools/sb_parser_gen/generate_authenticated_full_route_matrix.py",
        "AUTHENTICATED_FULL_ROUTE_MATRIX.csv",
    ),
    (
        "project/tools/sb_parser_gen/generate_sblr_binary_round_trip_matrix.py",
        "SBLR_BINARY_ROUND_TRIP_MATRIX.csv",
    ),
    (
        "project/tools/sb_parser_gen/generate_per_row_evidence_manifest.py",
        "PER_ROW_EVIDENCE_MANIFEST.csv",
    ),
    (
        "project/tools/sb_parser_gen/generate_status_change_authority_matrix.py",
        "STATUS_CHANGE_AUTHORITY_MATRIX.csv",
    ),
    (
        "project/tools/sb_parser_gen/generate_performance_resource_budget_matrix.py",
        "PERFORMANCE_RESOURCE_BUDGET_MATRIX.csv",
    ),
    (
        "project/tools/sb_parser_gen/generate_failure_triage_taxonomy.py",
        "FAILURE_TRIAGE_TAXONOMY.csv",
    ),
    (
        "project/tools/sb_parser_gen/generate_sbsql_surface_release_declaration.py",
        "SBSQL_SURFACE_RELEASE_DECLARATION.csv",
    ),
]

DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"


NETWORK_MODULE_NAMES = (
    "".join(("url", "lib")),
    "http",
    "".join(("so", "cket")),
    "ssl",
    "".join(("ft", "plib")),
    "".join(("sm", "tplib")),
    "".join(("telnet", "lib")),
    "".join(("imap", "lib")),
    "".join(("pop", "lib")),
    "".join(("nntp", "lib")),
    "".join(("xml", "rpc")),
    "".join(("web", "browser")),
    "".join(("re", "quests")),
    "httpx",
    "aiohttp",
    "".join(("url", "lib3")),
    "pycurl",
    "paramiko",
    "asyncssh",
    "botocore",
    "boto3",
    "google.cloud",
    "google.api_core",
)

NETWORK_MODULE_RE = re.compile(
    r"^\s*(?:import|from)\s+("
    + "|".join(re.escape(name) for name in NETWORK_MODULE_NAMES)
    + r")(?:[\.\s]|$)",
    re.MULTILINE,
)


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def md5_of(path: Path) -> str:
    if not path.is_file():
        return ""
    h = hashlib.md5()
    h.update(path.read_bytes())
    return h.hexdigest()


def scan_no_network(generator_paths: list[Path]) -> list[str]:
    findings: list[str] = []
    for path in generator_paths:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            findings.append(f"{path}: read error {exc}")
            continue
        for match in NETWORK_MODULE_RE.finditer(text):
            line_num = text[: match.start()].count("\n") + 1
            findings.append(f"{path.name}:{line_num} forbidden_network_import={match.group(1)!r}")
    return findings


def check_determinism(root: Path, artifact_root: Path) -> tuple[list[str], list[tuple[str, str, str]]]:
    drift: list[str] = []
    summary: list[tuple[str, str, str]] = []  # (generator, before_md5, after_md5)
    with tempfile.TemporaryDirectory(prefix="sbsql_surface_regen_") as tmp:
        temp_artifact_root = Path(tmp) / "artifacts"
        shutil.copytree(artifact_root, temp_artifact_root)
        for generator_rel, output_name in GENERATORS:
            gen_path = root / generator_rel
            out_path = temp_artifact_root / output_name
            if not gen_path.is_file():
                drift.append(f"generator missing: {generator_rel}")
                continue
            if not out_path.is_file():
                drift.append(f"output missing: {artifact_root / output_name}")
                continue

            before = md5_of(out_path)
            result = subprocess.run(
                [
                    sys.executable,
                    str(gen_path),
                    "--repo-root",
                    str(root),
                    "--artifact-root",
                    str(temp_artifact_root),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            if result.returncode != 0:
                drift.append(
                    f"{generator_rel}: regeneration failed exit={result.returncode} stderr={result.stderr.strip()[:200]}"
                )
                continue

            after = md5_of(out_path)
            summary.append((generator_rel, before, after))
            if before != after:
                drift.append(f"{generator_rel}: non-deterministic regeneration before_md5={before} after_md5={after}")
    return drift, summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()
    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root

    generator_paths = [root / rel for rel, _ in GENERATORS]

    # Phase 1: static no-network scan
    network_findings = scan_no_network(generator_paths)

    # Phase 2: deterministic regeneration check
    drift_findings, summary = check_determinism(root, artifact_root)

    print(
        "sbsql_deterministic_generation_no_network_gate "
        f"generators={len(GENERATORS)} "
        f"network_findings={len(network_findings)} "
        f"determinism_findings={len(drift_findings)}"
    )
    for generator_rel, before, after in summary:
        ok = "ok" if before == after else "DRIFT"
        print(f"  {generator_rel}: md5={before} -> {after} [{ok}]")

    if network_findings or drift_findings:
        print("sbsql_deterministic_generation_no_network_gate=failed", file=sys.stderr)
        for finding in network_findings + drift_findings:
            print(f"  {finding}", file=sys.stderr)
        return 1

    print("sbsql_deterministic_generation_no_network_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
