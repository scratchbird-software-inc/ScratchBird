#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""FSPE-012C deterministic generation and no-network gate."""

from __future__ import annotations

import argparse
import csv
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys


REGISTRY_OUTPUTS = (
    "sbsql_generated_registry.hpp",
    "sbsql_generated_registry.cpp",
    "sbsql_generated_registry.manifest",
)

REPLAY_OUTPUTS = (
    "DIFFERENTIAL_REPLAY_FIXTURE_INDEX.csv",
    "DIFFERENTIAL_REPLAY_EXPECTED_PAYLOADS.jsonl",
)

NETWORK_TOKENS = (
    "urllib.request",
    "http.client",
    "ftplib",
    "telnetlib",
    "requests.",
    "requests ",
    "http://",
    "https://",
    "curl ",
    "wget ",
    "git clone",
    "pip install",
    "npm install",
    "cargo fetch",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--update-manifest", action="store_true")
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def rel(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def remove_allowed_no_network_metadata(text: str) -> str:
    allowed_url_metadata_tokens = (
        "mozilla.org/MPL/2.0",
        '"source_url"',
        '"$schema"',
    )
    guardrail_depth = 0
    lines: list[str] = []
    for line in text.splitlines():
        stripped = line.strip()
        if guardrail_depth > 0:
            guardrail_depth += line.count("(") - line.count(")")
            if guardrail_depth <= 0:
                guardrail_depth = 0
            continue
        lhs = stripped.split("=", 1)[0].strip() if "=" in stripped else ""
        is_allowed_guardrail = (
            lhs.isupper()
            and (
                "NETWORK" in lhs
                or lhs == "FORBIDDEN_LOCATORS"
            )
        )
        if is_allowed_guardrail:
            guardrail_depth = line.count("(") - line.count(")")
            if guardrail_depth < 0:
                guardrail_depth = 0
            continue
        if any(token in line for token in allowed_url_metadata_tokens):
            continue
        lines.append(line)
    return "\n".join(
        lines
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run_generator(repo_root: Path, artifact_root: Path, output_dir: Path) -> None:
    generator = repo_root / "project/tools/sb_parser_gen/generate_sbsql_registry.py"
    env = os.environ.copy()
    env.update(
        {
            "LC_ALL": "C",
            "TZ": "UTC",
            "PYTHONHASHSEED": "0",
            "PYTHONNOUSERSITE": "1",
            "http_proxy": "",
            "https_proxy": "",
            "HTTP_PROXY": "",
            "HTTPS_PROXY": "",
            "ALL_PROXY": "",
            "NO_PROXY": "*",
        }
    )
    subprocess.run(
        [
            sys.executable,
            str(generator),
            "--artifact-root",
            str(artifact_root),
            "--output-dir",
            str(output_dir),
        ],
        cwd=repo_root,
        env=env,
        check=True,
    )


def run_replay_generator(repo_root: Path, artifact_root: Path, output_dir: Path) -> None:
    generator = repo_root / "project/tools/sb_parser_gen/generate_differential_replay_fixtures.py"
    env = os.environ.copy()
    env.update(
        {
            "LC_ALL": "C",
            "TZ": "UTC",
            "PYTHONHASHSEED": "0",
            "PYTHONNOUSERSITE": "1",
            "http_proxy": "",
            "https_proxy": "",
            "HTTP_PROXY": "",
            "HTTPS_PROXY": "",
            "ALL_PROXY": "",
            "NO_PROXY": "*",
        }
    )
    subprocess.run(
        [
            sys.executable,
            str(generator),
            "--repo-root",
            str(repo_root),
            "--artifact-root",
            str(artifact_root),
            "--replay-root",
            str(output_dir),
        ],
        cwd=repo_root,
        env=env,
        check=True,
    )


def compare_tree(left: Path, right: Path, names: tuple[str, ...]) -> None:
    for name in names:
        left_file = left / name
        right_file = right / name
        require(left_file.read_bytes() == right_file.read_bytes(),
                f"deterministic output mismatch for {name}")


def scan_no_network(repo_root: Path) -> None:
    scan_roots = (
        repo_root / "project/tools/sb_parser_gen",
        repo_root / "project/tests/sbsql_parser_worker/generated",
        repo_root / "project/tests/sbsql_parser_worker/CMakeLists.txt",
    )
    files: list[Path] = []
    for root in scan_roots:
        if root.is_file():
            files.append(root)
            continue
        for path in root.rglob("*"):
            if "/generated/repro/" in path.as_posix():
                continue
            if path.is_file() and path.suffix in {".py", ".cpp", ".hpp", ".txt", ".csv", ".jsonl"}:
                files.append(path)

    for path in files:
        text = remove_allowed_no_network_metadata(read_text(path))
        lowered = text.lower()
        for token in NETWORK_TOKENS:
            require(token not in lowered,
                    f"network/dependency token {token!r} found in {rel(path, repo_root)}")


def artifact_category(path: Path) -> str:
    parts = path.parts
    if "registry" in parts and "generated" in parts:
        return "registry_generated"
    if "generated" in parts:
        index = parts.index("generated")
        return "test_generated_" + (parts[index + 1] if index + 1 < len(parts) else "root")
    return "generated"


def source_inputs(path: Path) -> str:
    parts = path.parts
    if "registry" in parts and "generated" in parts:
        return "SURFACE_IMPLEMENTATION_BACKLOG.csv;BATCH_ROW_MEMBERSHIP.csv;SEMANTIC_ORACLE_AUTHORITY_MAP.csv"
    return "repo_tracked_generated_fixture"


def generated_artifacts(repo_root: Path) -> list[dict[str, str]]:
    roots = (
        repo_root / "project/src/parsers/sbsql_worker/registry/generated",
        repo_root / "project/tests/sbsql_parser_worker/generated",
    )
    files: list[Path] = []
    for root in roots:
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            if "__pycache__" in path.parts or path.suffix == ".pyc":
                continue
            if "/generated/repro/" in path.as_posix():
                continue
            files.append(path)

    rows = []
    for path in sorted(files, key=lambda item: rel(item, repo_root)):
        rows.append(
            {
                "artifact_path": rel(path, repo_root),
                "sha256": sha256(path),
                "size_bytes": str(path.stat().st_size),
                "category": artifact_category(path),
                "source_inputs": source_inputs(path),
            }
        )
    return rows


def read_manifest(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def write_manifest(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=("artifact_path", "sha256", "size_bytes", "category", "source_inputs"),
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    artifact_root = args.artifact_root.resolve()
    work_dir = args.work_dir.resolve()

    if work_dir.exists():
        shutil.rmtree(work_dir)
    pass1 = work_dir / "pass1"
    pass2 = work_dir / "pass2"

    run_generator(repo_root, artifact_root, pass1)
    run_generator(repo_root, artifact_root, pass2)
    compare_tree(pass1, pass2, REGISTRY_OUTPUTS)
    compare_tree(
        pass1,
        repo_root / "project/src/parsers/sbsql_worker/registry/generated",
        REGISTRY_OUTPUTS,
    )

    replay1 = work_dir / "replay_pass1"
    replay2 = work_dir / "replay_pass2"
    run_replay_generator(repo_root, artifact_root, replay1)
    run_replay_generator(repo_root, artifact_root, replay2)
    compare_tree(replay1, replay2, REPLAY_OUTPUTS)
    compare_tree(
        replay1,
        repo_root / "project/tests/sbsql_parser_worker/generated/replay",
        REPLAY_OUTPUTS,
    )

    scan_no_network(repo_root)

    runtime_manifest = generated_artifacts(repo_root)
    write_manifest(work_dir / "runtime_manifest.csv", runtime_manifest)
    if args.update_manifest:
        write_manifest(args.manifest, runtime_manifest)
    expected_manifest = read_manifest(args.manifest)
    require(runtime_manifest == expected_manifest,
            "deterministic artifact manifest does not match tracked generated artifacts")

    print(
        "FSPE-012C deterministic no-network gate passed: "
        f"registry_files={len(REGISTRY_OUTPUTS)} generated_artifacts={len(runtime_manifest)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
